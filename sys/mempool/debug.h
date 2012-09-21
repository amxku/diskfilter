/*
 * Copyright (C) 2001-2004 The Honeynet Project.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *      This product includes software developed by The Honeynet Project.
 * 4. The name "The Honeynet Project" may not be used to endorse or promote
 *    products derived from this software without specific prior written
 *    permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifndef DEBUG_H
#define DEBUG_H

/*
 * Debugging-only macros to write messages to the debug log and optionally
 * cause a breakpoint.
 */
#if DBG
#define DBGOUT(params)				\
	{					\
		DbgPrint ("[sys] ");	\
		DbgPrint params;		\
		DbgPrint ("\n");		\
	}
#define TRAP(msg)				\
	{					\
		DBGOUT(("TRAP at file %s, line %d: '%s'.", __FILE__, __LINE__, msg)); \
		DbgBreakPoint();		\
  }

void DbgPrintHexDump(unsigned char	*pBuffer,	unsigned long	Length);

#define DEBUGPDUMP(pBuf, Len)                                      \
	{                                                            \
		DbgPrintHexDump((PUCHAR)(pBuf), (ULONG)(Len));          \
	}

#else
#define DBGOUT(params)
#define TRAP(msg)
#define DEBUGPDUMP(pBuf, Len) 
#endif

#endif
