/*
 * disk.c
 * simple CADR disk emulation
 * attempts to emulate the disk controller on a CADR
 *
 * $Id$
 */

#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>

#include "ucode.h"

/*
	disk controller registers:
	  0 read status
	  1 read ma
	  2 read da
	  3 read ecc
	  4 load cmd
	  5 load clp (command list pointer)
	  6 load da (disk address)
	  7 start

	Commands (cmd reg)
	  00 read
	  10 read compare
	  11 write
	  02 read all
	  13 write all
	  04 seek
	  05 at ease
	  1005 recalibreate
	  405 fault clear
	  06 offset clear
	  16 stop,reset

	Command bits
	  0
	  1 cmd
	  2
	  3 cmd to memory
	  4 servo offset plus
	  5 servo offset
	  6 data strobe early
	  7 data strobe late
	  8 fault clear
	  9 recalibrate
	  10 attn intr enb
	  11 done intr enb

	Status bits (status reg)
	  0 active-
	  1 any attention
	  2 sel unit attention
	  3 intr
	  4 multiple select
	  5 no select
	  6 sel unit fault
	  7 sel unit read only
	  8 on cyl sync-
	  9 sel unit on line-
	  10 sel unit seek error
	  11 timeout error
	  12 start block error
	  13 stopped by error
	  14 overrun
	  15 ecc.soft

	  16 ecc.hard
	  17 header ecc err
	  18 header compare err
	  19 mem parity err
	  20 nmx error
	  21 ccw cyc
	  22 read comp diff
	  23 internal parity err
	  
	  24-31 block.ctr

	Disk address (da reg)
	  31 n/c
	  30 unit2
	  29 unit1
	  28 unit0

	  27 cyl11
	  ...
	  16 cyl0	  
	  
	  15 head7
	  ...
	  8  head0

	  7  block7
	  ...
	  0  block0

	  ---

	  CLP (command list pointer) points to list of CCW's
	  Each CCW is phy address to write block

	  clp register (22 bits)
	  [21:16][15:0]
	  fixed  counts up

	  clp address is used to read in new ccw
	  ccw's are read (up to 65535)

	  ccw is used to produce dma address
	  dma address comes from ccw + 8 bit counter

	  ccw
	  [21:1][1]
          physr  |
	  addr   0 = last ccw, 1 = more ccw's

	  ccw   counter
	  [21:8][7:0]

	  ---

	  read ma register
	   t0  t1 CLP
	  [23][22][21:0]
            |   |
            |   type 1 (show how controller is strapped; i.e. what type of
            type 0      disk drive)

	    (trident is type 0)


*/

int disk_fd;

int disk_status = 1;
int disk_cmd;
int disk_clp;
int disk_da;

int
disk_get_status(void)
{
	return disk_status;
}

int
disk_set_da(int v)
{
	disk_da = v;
}

int
disk_set_clp(int v)
{
	disk_clp = v;
}

int
disk_set_cmd(int v)
{
	disk_cmd = v;
}

int cyls, heads, blocks_per_track;
int cur_unit, cur_cyl, cur_head, cur_block;

void
_swaplongbytes(unsigned int *buf)
{
	int i;
#if 0
	unsigned char *p = (unsigned char *)buf;

	for (i = 0; i < 256*4; i += 4) {
		unsigned char t;
		t = p[i];
		p[i] = p[i+1];
		p[i+1] = t;
	}
#endif
#if 0
	for (i = 0; i < 256; i++) {
		buf[i] = ntohl(buf[i]);
	}
#endif
#if 1
	unsigned short *p = (unsigned short *)buf;

	for (i = 0; i < 256*2; i += 2) {
		unsigned short t;
		t = p[i];
		p[i] = p[i+1];
		p[i+1] = t;
	}
#endif
}

int
_disk_read(int block_no, unsigned int *buffer)
{
	off_t offset, ret;
	int size;

	offset = block_no * (256*4);

	if (1) printf("disk: file image block %d, offset %ld\n", block_no, offset);

	ret = lseek(disk_fd, offset, SEEK_SET);
	if (ret != offset) {
		printf("disk: image file seek error\n");
		perror("lseek");
		return -1;
	}

	size = 256*4;

	ret = read(disk_fd, buffer, size);
	if (ret != size) {
		printf("disk read error; ret %d, size %d\n", ret, size);
		perror("read");
		return -1;
	}

	/* byte order fixups? */
	_swaplongbytes((unsigned int *)buffer);

	return 0;
}

int
disk_read_block(unsigned int vma, int unit, int cyl, int head, int block)
{
	int block_no, i;
	unsigned int buffer[256];

	block_no =
		(cyl * blocks_per_track * heads) +
		(head * blocks_per_track) + block;

	if (disk_fd) {
		_disk_read(block_no, buffer);
		for (i = 0; i < 256; i++) {
			write_mem(vma + i, buffer[i]);
		}
		return 0;
	}

	/* hack to fake a disk label when no image is present */
	if (unit == 0 && cyl == 0 && head == 0 && block == 0) {
		write_mem(vma + 0, 011420440514); /* label LABL */
		write_mem(vma + 1, 000000000001); /* version = 1 */
		write_mem(vma + 2, 000000001000); /* # cyls */
		write_mem(vma + 3, 000000000004); /* # heads */
		write_mem(vma + 4, 000000000100); /* # blocks */
		write_mem(vma + 5, 000000000400); /* heads*blocks */
		write_mem(vma + 6, 000000001234); /* name of micr part */
		write_mem(vma + 0200, 1); /* # of partitions */
		write_mem(vma + 0201, 1); /* words / partition */

		write_mem(vma + 0202, 01234); /* start of partition info */
		write_mem(vma + 0203, 01000); /* micr address */
		write_mem(vma + 0204, 010);   /* # blocks */
		/* pack text label - offset 020, 32 bytes */
		return 0;
	}
}

void
disk_throw_interrupt(void)
{
	printf("disk: throw interrupt\n");
	post_xbus_interrupt();
}

void
disk_show_cur_addr(void)
{
	printf("disk: unit %d, CHB %o/%o/%o\n",
	       cur_unit, cur_cyl, cur_head, cur_block);
}

void
disk_decode_addr(void)
{
	cur_unit = (disk_da >> 28) & 07;
	cur_cyl = (disk_da >> 16) & 07777;
	cur_head = (disk_da >> 8) & 0377;
	cur_block = disk_da & 0377;
}

void
disk_incr_block(void)
{
	cur_block++;
	if (cur_block > blocks_per_track) {
		cur_block = 0;
		cur_head++;
		if (cur_head > heads) {
			cur_head = 0;
			cur_cyl++;
		}
	}
}

void
disk_start_read(void)
{
	unsigned int ccw;
	unsigned int vma;
	int i;

	disk_decode_addr();

	/* process ccw's */
	for (i = 0; i < 65535; i++) {
		int f;

		f = read_phy_mem(disk_clp, &ccw);
		if (f) {
			printf("disk: mem[clp=%o] yielded fault (no page)\n",
			       disk_clp);

			/* huh.  what to do now? */
			return;
		}

		printf("disk: mem[clp=%o] -> ccw %08o\n", disk_clp, ccw);

		vma = ccw & ~0377;

		disk_show_cur_addr();

		disk_read_block(vma, cur_unit, cur_cyl, cur_head, cur_block);

		disk_incr_block();
			
		if ((ccw & 1) == 0) {
			printf("disk: last ccw\n");
			break;
		}

		disk_clp++;
	}

	if (disk_cmd & 04000) {
		disk_throw_interrupt();
	}
}

void
disk_start_read_compare(void)
{
	disk_decode_addr();
	disk_show_cur_addr();
}

void
disk_start_write(void)
{
	disk_decode_addr();
	disk_show_cur_addr();
}

int
disk_start(void)
{
	printf("disk: start, cmd ");

	switch (disk_cmd & 01777) {
	case 0:
		printf("read\n");
		disk_start_read();
		break;
	case 010:
		printf("read compare\n");
		disk_start_read_compare();
		break;
	case 011:
		printf("write\n");
		disk_start_write();
		break;
	case 01005:
		printf("recalibrate\n");
		break;
	case 0405:
		printf("fault clear\n");
		break;
	default:
		printf("unknown\n");
	}
}

int
disk_init(char *filename)
{
	unsigned int label[256];

	disk_fd = open(filename, O_RDWR);
	if (disk_fd < 0) {
		disk_fd = 0;
		perror(filename);
		return -1;
	}

	_disk_read(0, label);
	if (label[0] != 011420440514) {
		printf("disk: invalid pack label - disk image ignored");
		close(disk_fd);
		disk_fd = 0;
	}

	cyls = label[2];
	heads = label[3];
	blocks_per_track = label[4];

	printf("disk: image CHB %o/%o/%o\n", cyls, heads, blocks_per_track);

	return 0;
}