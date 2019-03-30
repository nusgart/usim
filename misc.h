#ifndef USIM_MISC_H
#define USIM_MISC_H

#include <stdint.h>

#ifndef O_BINARY
#define O_BINARY 0
#endif

extern void dumpmem(char *ptr, int len);

extern uint16_t read16(int fd);
extern uint32_t read32(int fd);

extern unsigned long str4(char *s);
extern char *unstr4(unsigned long s);

#define BLOCKSZ (256 * 4)

extern int read_block(int fd, int block_no, unsigned char *buf);
extern int write_block(int fd, int block_no, unsigned char *buf);

extern uint32_t load_byte(uint32_t w, int p, int s);
extern uint32_t deposit_byte(uint32_t w, int p, int s, uint32_t v);
extern uint32_t ldb(int ppss, uint32_t w);
extern uint32_t dpb(uint32_t v, int ppss, uint32_t w);

#endif
