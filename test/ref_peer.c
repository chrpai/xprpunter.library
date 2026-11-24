/*
 * ref_peer.c - reference Punter C1 peer used ONLY for interoperability tests.
 *
 * Adapted from CGTerm punter.c (as modified for CCGMS Future by Michael Steil)
 *   Copyright (c) 2003,2016, Per Olofsson. All rights reserved.
 *   https://github.com/MagerValp/CGTerm   -- BSD 2-Clause License
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES ARE DISCLAIMED.  IN NO EVENT SHALL THE
 * COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DAMAGES ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE.
 *
 * Changes: stdio tracing removed, I/O routed through function pointers,
 * time scaled for fast tests, data kept in memory.  The documented upstream
 * quirk that blocks with len <= 8 never save their payload is preserved.
 */
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include "ref_peer.h"

static unsigned char xfer_buffer[4096];
static int (*in_fn)(int timeout);
static void (*out_fn)(int c);
static void (*sleep_fn)(int sec);
static unsigned char *save_ptr; static int save_len;

static void xfer_send_byte(unsigned char c){ out_fn(c); }
static signed int xfer_recv_byte(int t){ return in_fn(t); }
#define xfer_recv_byte_error(t,e) xfer_recv_byte(t)
static int xfer_save_data(unsigned char *d, int n){ memcpy(save_ptr+save_len,d,n); save_len+=n; return 1; }
static int xfer_cancel = 0;
#define MIN(a,b) ((a)<(b)?(a):(b))

static void punter_send_string(char *s){ while(*s) xfer_send_byte(*s++); }

static int punter_recv_string(char *sendstring, char *recvstring){
  int c, bytecnt; int errorcnt;
  memset(recvstring,'a',4);
  if (sendstring) punter_send_string(sendstring);
  bytecnt=0; errorcnt=10;
  while (bytecnt!=3){
    while ((c=xfer_recv_byte(1000))<0 && errorcnt-- && (xfer_cancel==0)){
      if (sendstring) punter_send_string(sendstring);
      bytecnt=0;
    }
    if (errorcnt && xfer_cancel==0) recvstring[bytecnt++]=c; else return 0;
  }
  recvstring[3]=0; return 3;
}

static int punter_handshake(char *sendstring, char *waitstring){
  char p[4]; int l=0; int errorcnt=10;
  while(errorcnt--){
    l=punter_recv_string(sendstring,p);
    if(l==3){
      if(strcmp(waitstring,p)==0) return 1;
      else if(strcmp(sendstring,p)==0) return 0;
      else if(strcmp("ACK",waitstring)==0){
        if(strcmp("CKA",p)==0){ xfer_recv_byte(100); xfer_recv_byte(100); return 1; }
        if(strcmp("KAC",p)==0){ xfer_recv_byte(100); return 1; }
      }
    }
  }
  return 0;
}

static void punter_checksum(int len, unsigned short *c1, unsigned short *c2){
  unsigned short cksum=0, clc=0; unsigned char *data=xfer_buffer+4; len-=4;
  while(len--){ cksum+=*data; clc^=*data++; clc=(clc<<1)|(clc>>15); }
  *c1=cksum; *c2=clc;
}
static int punter_checksum_verify(int len){
  unsigned short cksum, clc; punter_checksum(len,&cksum,&clc);
  return cksum==(xfer_buffer[0]|(xfer_buffer[1]<<8)) && clc==(xfer_buffer[2]|(xfer_buffer[3]<<8));
}
static void punter_checksum_create(int len){
  unsigned short cksum, clc; punter_checksum(len,&cksum,&clc);
  xfer_buffer[0]=cksum&0xff; xfer_buffer[1]=cksum>>8; xfer_buffer[2]=clc&0xff; xfer_buffer[3]=clc>>8;
}
static unsigned short punter_next_blocknum(void){ return xfer_buffer[5]|(xfer_buffer[6]<<8); }

static signed int punter_recv_block(int len){
  signed int c; int bytecnt; int errorcnt=10;
restart:
  punter_send_string("S/B");
  bytecnt=0;
  while(bytecnt<len){
    if((c=xfer_recv_byte_error(500,10))<0){
      if(bytecnt==3 && strncmp("S/B",(char*)xfer_buffer,3)==0) return -1;
      if(punter_handshake("BAD","ACK")){ if(errorcnt--) goto restart; else return -1; }
      else return -1;
    }
    xfer_buffer[bytecnt++]=c;
    if(bytecnt==4){
      if(strncmp("ACK",(char*)xfer_buffer,3)==0){
        if(xfer_buffer[3]=='A') goto restart;
        else { xfer_buffer[0]=xfer_buffer[3]; bytecnt=1; }
      }
    }
    if(bytecnt==8){
      if(strncmp("ACKACK",(char*)xfer_buffer+2,6)==0) goto restart;
      if(strncmp("CKACKA",(char*)xfer_buffer+2,6)==0) goto restart;
      if(strncmp("KACKAC",(char*)xfer_buffer+2,6)==0) goto restart;
    }
  }
  if(punter_checksum_verify(bytecnt)){
    if(punter_handshake("GOO","ACK")==0) return -1;
    if(len<=8) return xfer_buffer[4];
    if(xfer_save_data(xfer_buffer+7,len-7)) return xfer_buffer[4]; else return -1;
  } else {
    if(punter_handshake("BAD","ACK")){ if(errorcnt--) goto restart; else return -1; }
    else return -1;
  }
}

int ref_recv(int (*in)(int), void (*out)(int), void (*slp)(int), unsigned char *dst, int *outlen)
{
  signed int nextblocksize; char p[4]; int l;
  in_fn=in; out_fn=out; sleep_fn=slp; save_ptr=dst; save_len=0;
  l=punter_recv_string(NULL,p); (void)l;
  if(!punter_handshake("GOO","ACK")) return 0;
  nextblocksize=punter_recv_block(8); if(nextblocksize<0) return 0;
  if(!punter_handshake("GOO","ACK")) return 0;
  if(!punter_handshake("S/B","SYN")) return 0;
  if(!punter_handshake("SYN","S/B")) return 0;
  l=punter_recv_string(NULL,p); l=punter_recv_string(NULL,p); (void)l;
  sleep_fn(2);
  if(!punter_handshake("GOO","ACK")) return 0;
  nextblocksize=punter_recv_block(7); if(nextblocksize<0) return 0;
  while(punter_next_blocknum()<0xff00 && nextblocksize>=7) nextblocksize=punter_recv_block(nextblocksize);
  if(nextblocksize<0) return 0;
  if(punter_handshake("S/B","SYN")) punter_handshake("SYN","S/B");
  *outlen=save_len; return 1;
}

static void xmit_block(int len){ for(int i=0;i<len;i++) xfer_send_byte(xfer_buffer[i]); }

int ref_xmit(int (*in)(int), void (*out)(int), void (*slp)(int), unsigned char *data, int data_len)
{
  in_fn=in; out_fn=out; sleep_fn=slp;
  punter_handshake("GOO","GOO");
  punter_handshake("ACK","S/B");
  xfer_buffer[5]=0xff; xfer_buffer[6]=0xff; xfer_buffer[7]=1;
  punter_checksum_create(8); xmit_block(8);
  punter_handshake(NULL,"GOO");
  punter_handshake("ACK","S/B");
  punter_handshake("SYN","SYN");
  punter_send_string("S/B");
  punter_handshake(NULL,"GOO");
  int max_block=255, index=0, data_ptr=0, len=7, next_len, is_last_block=0;
  do {
    int next_data_ptr=data_ptr+len-7;
    next_len=MIN(data_len-next_data_ptr,max_block-7)+7;
    if(len<max_block && index){ index=0xffff; is_last_block=1; }
    punter_handshake("ACK","S/B");
    xfer_buffer[5]=index&0xff; xfer_buffer[6]=index>>8; xfer_buffer[4]=next_len;
    memcpy(&xfer_buffer[7],&data[data_ptr],len-7);
    punter_checksum_create(len); xmit_block(len);
    punter_handshake(NULL,"GOO");
    index++; data_ptr=next_data_ptr; len=next_len;
  } while(!is_last_block);
  punter_handshake("ACK","S/B");
  punter_handshake("SYN","SYN");
  punter_send_string("S/B");
  return 1;
}
