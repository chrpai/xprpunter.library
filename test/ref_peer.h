#ifndef REF_PEER_H
#define REF_PEER_H
int ref_recv(int (*in)(int), void (*out)(int), void (*slp)(int), unsigned char *dst, int *outlen);
int ref_xmit(int (*in)(int), void (*out)(int), void (*slp)(int), unsigned char *data, int data_len);
#endif
