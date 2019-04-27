#ifndef USIM_UTRACE_H
#define USIM_UTRACE_H

#define TRACE_MISC	0001 // 0000_0000_0001
#define TRACE_VM	0002 // 0000_0000_0010
#define TRACE_INT	0004 // 0000_0000_1000
#define TRACE_DISK	0010 // 0000_0001_0000
#define TRACE_CHAOS	0020 // 0000_0100_0000
#define TRACE_IOB	0040 // 0000_1000_0000
#define TRACE_MICROCODE	0100 // 0001_0000_0000
#define TRACE_MACROCODE	0200 // 0010_0000_0000

#define TRACE_ALL 	01777	// 0011_1111_1111

#include "trace.h"

#endif