/* Ppmd7Dec.c -- PPMdH Decoder
2010-03-12 : Igor Pavlov : Public domain
This code is based on PPMd var.H (2001): Dmitry Shkarin : Public domain */

#include "archive_platform.h"

#include "archive_ppmd7_private.h"

#define kTopValue (1 << 24)

static Bool Ppmd_RangeDec_Init(CPpmd7z_RangeDec *p)
{
  unsigned i;
  p->Low = p->Bottom = 0;
  p->Range = 0xFFFFFFFF;
  for (i = 0; i < 4; i++)
    p->Code = (p->Code << 8) | p->Stream->Read((void *)p->Stream);
  return (p->Code < 0xFFFFFFFF);
}

Bool Ppmd7z_RangeDec_Init(CPpmd7z_RangeDec *p)
{
  if (p->Stream->Read((void *)p->Stream) != 0)
    return False;
  return Ppmd_RangeDec_Init(p);
}

Bool PpmdRAR_RangeDec_Init(CPpmd7z_RangeDec *p)
{
  if (!Ppmd_RangeDec_Init(p))
    return False;
  p->Bottom = 0x8000;
  return True;
}

static UInt32 Range_GetThreshold(void *pp, UInt32 total)
{
  CPpmd7z_RangeDec *p = (CPpmd7z_RangeDec *)pp;
  return (p->Code - p->Low) / (p->Range /= total);
}

static void Range_Normalize(CPpmd7z_RangeDec *p)
{
  while (1)
  {
    if((p->Low ^ (p->Low + p->Range)) >= kTopValue)
    {
      if(p->Range >= p->Bottom)
        break;
      else
        p->Range = -p->Low & (p->Bottom - 1);
    }
    p->Code = (p->Code << 8) | p->Stream->Read((void *)p->Stream);
    p->Range <<= 8;
    p->Low <<= 8;
  }
}

static void Range_Decode_7z(void *pp, UInt32 start, UInt32 size)
{
  CPpmd7z_RangeDec *p = (CPpmd7z_RangeDec *)pp;
  p->Code -= start * p->Range;
  p->Range *= size;
  Range_Normalize(p);
}

static void Range_Decode_RAR(void *pp, UInt32 start, UInt32 size)
{
  CPpmd7z_RangeDec *p = (CPpmd7z_RangeDec *)pp;
  p->Low += start * p->Range;
  p->Range *= size;
  Range_Normalize(p);
}

static UInt32 Range_DecodeBit_7z(void *pp, UInt32 size0)
{
  CPpmd7z_RangeDec *p = (CPpmd7z_RangeDec *)pp;
  UInt32 newBound = (p->Range >> 14) * size0;
  UInt32 symbol;
  if (p->Code < newBound)
  {
    symbol = 0;
    p->Range = newBound;
  }
  else
  {
    symbol = 1;
    p->Code -= newBound;
    p->Range -= newBound;
  }
  Range_Normalize(p);
  return symbol;
}

static UInt32 Range_DecodeBit_RAR(void *pp, UInt32 size0)
{
  CPpmd7z_RangeDec *p = (CPpmd7z_RangeDec *)pp;
  UInt32 bit, value = p->p.GetThreshold(p, PPMD_BIN_SCALE);
  if(value < size0)
  {
    bit = 0;
    p->p.Decode(p, 0, size0);
  }
  else
  {
    bit = 1;
    p->p.Decode(p, size0, PPMD_BIN_SCALE - size0);
  }
  return bit;
}

void Ppmd7z_RangeDec_CreateVTable(CPpmd7z_RangeDec *p)
{
  p->p.GetThreshold = Range_GetThreshold;
  p->p.Decode = Range_Decode_7z;
  p->p.DecodeBit = Range_DecodeBit_7z;
}

void PpmdRAR_RangeDec_CreateVTable(CPpmd7z_RangeDec *p)
{
  p->p.GetThreshold = Range_GetThreshold;
  p->p.Decode = Range_Decode_RAR;
  p->p.DecodeBit = Range_DecodeBit_RAR;
}

#define MASK(sym) ((signed char *)charMask)[sym]

int Ppmd7_DecodeSymbol(CPpmd7 *p, IPpmd7_RangeDec *rc)
{
  size_t charMask[256 / sizeof(size_t)];
  if (p->MinContext->NumStats != 1)
  {
    CPpmd_State *s = Ppmd7_GetStats(p, p->MinContext);
    unsigned i;
    UInt32 count, hiCnt;
    if ((count = rc->GetThreshold(rc, p->MinContext->SummFreq)) < (hiCnt = s->Freq))
    {
      Byte symbol;
      rc->Decode(rc, 0, s->Freq);
      p->FoundState = s;
      symbol = s->Symbol;
      Ppmd7_Update1_0(p);
      return symbol;
    }
    p->PrevSuccess = 0;
    i = p->MinContext->NumStats - 1;
    do
    {
      if ((hiCnt += (++s)->Freq) > count)
      {
        Byte symbol;
        rc->Decode(rc, hiCnt - s->Freq, s->Freq);
        p->FoundState = s;
        symbol = s->Symbol;
        Ppmd7_Update1(p);
        return symbol;
      }
    }
    while (--i);
    if (count >= p->MinContext->SummFreq)
      return -2;
    p->HiBitsFlag = p->HB2Flag[p->FoundState->Symbol];
    rc->Decode(rc, hiCnt, p->MinContext->SummFreq - hiCnt);
    PPMD_SetAllBitsIn256Bytes(charMask);
    MASK(s->Symbol) = 0;
    i = p->MinContext->NumStats - 1;
    do { MASK((--s)->Symbol) = 0; } while (--i);
  }
  else
  {
    UInt16 *prob = Ppmd7_GetBinSumm(p);
    if (rc->DecodeBit(rc, *prob) == 0)
    {
      Byte symbol;
      *prob = (UInt16)PPMD_UPDATE_PROB_0(*prob);
      symbol = (p->FoundState = Ppmd7Context_OneState(p->MinContext))->Symbol;
      Ppmd7_UpdateBin(p);
      return symbol;
    }
    *prob = (UInt16)PPMD_UPDATE_PROB_1(*prob);
    p->InitEsc = PPMD7_kExpEscape[*prob >> 10];
    PPMD_SetAllBitsIn256Bytes(charMask);
    MASK(Ppmd7Context_OneState(p->MinContext)->Symbol) = 0;
    p->PrevSuccess = 0;
  }
  for (;;)
  {
    CPpmd_State *ps[256], *s;
    UInt32 freqSum, count, hiCnt;
    CPpmd_See *see;
    unsigned i, num, numMasked = p->MinContext->NumStats;
    do
    {
      p->OrderFall++;
      if (!p->MinContext->Suffix)
        return -1;
      p->MinContext = Ppmd7_GetContext(p, p->MinContext->Suffix);
    }
    while (p->MinContext->NumStats == numMasked);
    hiCnt = 0;
    s = Ppmd7_GetStats(p, p->MinContext);
    i = 0;
    num = p->MinContext->NumStats - numMasked;
    do
    {
      int k = (int)(MASK(s->Symbol));
      hiCnt += (s->Freq & k);
      ps[i] = s++;
      i -= k;
    }
    while (i != num);
    
    see = Ppmd7_MakeEscFreq(p, numMasked, &freqSum);
    freqSum += hiCnt;
    count = rc->GetThreshold(rc, freqSum);
    
    if (count < hiCnt)
    {
      Byte symbol;
      CPpmd_State **pps = ps;
      for (hiCnt = 0; (hiCnt += (*pps)->Freq) <= count; pps++);
      s = *pps;
      rc->Decode(rc, hiCnt - s->Freq, s->Freq);
      Ppmd_See_Update(see);
      p->FoundState = s;
      symbol = s->Symbol;
      Ppmd7_Update2(p);
      return symbol;
    }
    if (count >= freqSum)
      return -2;
    rc->Decode(rc, hiCnt, freqSum - hiCnt);
    see->Summ = (UInt16)(see->Summ + freqSum);
    do { MASK(ps[--i]->Symbol) = 0; } while (i != 0);
  }
}
