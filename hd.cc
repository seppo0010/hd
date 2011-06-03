//
//	Simple hex-dump tool.  Copyright (c) 1995-2003 Jim Peters
//	<http://uazu.net>.  Released under the GNU GPL v2.
//

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <memory.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>

static void error(char *fmt, ...) { 
  va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n"); exit(1);
}
void usage(void) {
  error("Usage: hd [-r124] [files..]\n"
	"  -r         Real-time update\n"
	"  -a         Omit addresses at the start of lines\n"
	"  -d         Leave duplicate lines in\n"
	"  -1 -2 etc  Display `n' bytes per line\n"
	"  -t         Text only, with C-style escapes\n"
	);
}

// PROTOTYPES 

int main(int argc, char **argv);
void do_file(int fd);
static inline void out_flush();
static inline void out_line(char *bp, int bcnt);
static inline void hex(char *p, int val);
void do_file_text(int fd);
static inline void out_text_nl();

// CONSTANTS

static int const MAX_BPL= 32;		// Maximum bytes per line
static char hex_ch[]= "0123456789abcdef";
static char ctrl_ch[]= "       abtn fr             e    ";

// GLOBALS

int f_real= 0;		// Show bytes real-time
int f_addr= 8;		// Add address to lines (either 8 or 0: true or false)
int f_duprm= 1;		// Remove duplicate lines
int f_text= 0;		// Text mode ?
int n_byt= -1;		// Bytes per line
char *buf;		// Temporary read buffer
int buflen;		// Length of read buffer
char *obuf;		// Output buffer
int obuf_len;		// Output buffer length
char *obuf_end;		// End of output buffer +1
char *op;		// Next free byte in output buffer
char *obop;		// Output buffer output pointer (normally obuf, unless in real-time mode)
char *ocl;		// Start of current line
char *opp;		// Last character that can be printed if the line is partial
char pdat[MAX_BPL];	// Previous lines' data (for f_duprm)
int pdat_mod;		// Mode of pdat[]: 0 invalid, 1 valid+unused, 2 valid+used
int choff[MAX_BPL];	// Offsets to write characters in output line
int hxoff[MAX_BPL];	// Offsets to write hex numbers in output line	
int end_off;		// Offset of last character in line + 1
int addr;		// File address of current line
int m_len;		// For text mode - calculated maximum length of a line

//
//	Main function
//

int main(int argc, char **argv) {
  // Parse arguments
  argc--; argv++;
     
  while (argc > 0 && argv[0][0] == '-') {
    char c, *p= &argv[0][1];
    argc--; argv++;
    while (c= *p++) switch (c) {
     case 'r':
       f_real= 1; break;
     case 'a':
       f_addr= 0; break;
     case 'd':
       f_duprm= 0; break;
     case 't':
       f_text= 1; break;
     default:
       if (!isdigit(c)) usage();
       n_byt= c - '0';
       while (isdigit(*p)) n_byt= 10*n_byt + (*p++) - '0';
       break;
    }
  }

  // Checks
  if (n_byt == -1) n_byt= f_text ? (80 - f_addr) : 16;
  if (!f_text && (n_byt < 1 || n_byt > MAX_BPL))
    error("Invalid bytes per line: %d", n_byt);
  if (f_text && (n_byt < 4 || n_byt > 1024))
    error("Invalid bytes per line: %d", n_byt);

  // Init
  if (!(buf= (char*)malloc(buflen= 16384))) error("Out of memory");
  if (!(obuf= (char*)malloc(obuf_len= 16384))) error("Out of memory");
  op= obuf; obuf_end= obuf + obuf_len; obop= obuf;

  if (!f_text) {
    int off= f_addr;
    for (int a= 0; a<n_byt; a++) {
      hxoff[a]= off; off += 3;
      if ((a&7) == 7) off++;
    }
    off++;
    for (int a= 0; a<n_byt; a++) {
      choff[a]= off; off++;
    }
    end_off= off;
  }

  // Process input files
  if (argc == 0) do_file(fileno(stdin));

  while (argc > 0) {
    FILE *in= fopen(argv[0], "r");
    if (!in) error("Can't open file: %s", argv[0]);
    do_file(fileno(in));
    fclose(in);
    argc--, argv++;
  }
    
  exit(0);
}

//
//	Process all the data from one FD
//

void do_file(int fd) {
  int cnt;
  fd_set fset;
  char lbuf[MAX_BPL];
  char lbuf_cnt= 0;
  char *bp;

  // Text mode is sufficiently different that it is worth putting into
  // another function
  if (f_text) { do_file_text(fd); return; }
  
  pdat_mod= 0;
  addr= 0;

  if (f_real && fcntl(fd, F_SETFL, O_NONBLOCK))
    error("Can't set non-blocking input for real-time mode");

  while (0 != (cnt= read(fd, buf, buflen))) {
    if (cnt < 0) {
      if (errno != EAGAIN) error("Read error, errno==%d", errno);

      // We have to wait for more data - flush out the buffer if we
      // are in real-time mode.
      if (f_real) {
	if (lbuf_cnt == 0) 
	  out_flush();
	else {
	  // obop can only be >= op if there is already something
	  // partially displayed, in which case the partially displayed
	  // line will start at obuf anyway
	  if (obop < op) out_flush(); 
	  out_line(lbuf, lbuf_cnt); op= opp;
	  out_flush(); obop= opp;
	}
      }
	
      FD_ZERO(&fset);
      FD_SET(fd, &fset);
      if (-1 == select(fd+1, &fset, 0, &fset, 0)) error("Select failure");
      continue;
    }

    // Process tail-end of last read here
    bp= buf;
    if (lbuf_cnt > 0) {
      int len= n_byt - lbuf_cnt;
      if (len > cnt) len= cnt;
      memcpy(lbuf + lbuf_cnt, bp, len);
      bp += len; cnt -= len; lbuf_cnt += len;
      if (lbuf_cnt < n_byt) continue;
      out_line(lbuf, n_byt); addr += n_byt;
    }

    // Process bulk of read here
    while (cnt >= n_byt) {
      out_line(bp, n_byt); addr += n_byt;
      bp += n_byt; cnt -= n_byt;
    }

    // Hang onto trailing bytes for next read
    lbuf_cnt= cnt;
    if (cnt > 0)
      memcpy(lbuf, bp, cnt);
  }

  // Sort out trailing bytes
  if (lbuf_cnt > 0) {
    out_line(lbuf, lbuf_cnt); 
    addr += lbuf_cnt;
  }

  // Finish off
  if (f_addr) {
    out_line(lbuf, 1);
    op= ocl + 6; *op++= '\n';
  }
  out_flush();
}

//
//	Flush output buffer
//

static inline void out_flush() {
  int len= op-obop;
  while (len) {
     int rv= write(1, obop, len);
     if (rv < 0) error("Write error");
     obop += rv;
     len -= rv;
  }
  op= obop= obuf;
}

//
//	Output a line to the output buffer, flushing if necessary
//

static inline void out_line(char *bp, int bcnt) {
  if (op + end_off + 1 >= obuf_end)
    out_flush();

  if (f_duprm && pdat_mod && bcnt == n_byt &&
      0 == memcmp(pdat, bp, n_byt)) {
    if (pdat_mod == 1) {
      *op++= '*'; *op++= '\n';
      pdat_mod= 2;
    }
    return;
  }
  
  memcpy(pdat, bp, n_byt);
  pdat_mod= 1;

  memset(op, ' ', end_off);
  ocl= op;
  opp= op + hxoff[bcnt-1] + 2;

  if (f_addr) {
    hex(op, addr>>16);
    hex(op+2, addr>>8);
    hex(op+4, addr);
    op[6]= ':';
  }

  for (int a= 0; a<bcnt; a++) {
    int ch= bp[a] & 255;
    hex(op + hxoff[a], ch);
    op[choff[a]]= (ch < ' ' || ch >= 127) ? '.' : ch;
  }
  op += choff[bcnt-1]+1;
  *op++= '\n';
}

//
//	Write a byte in hex
//
  
static inline void hex(char *p, int val) {
  *p++= hex_ch[(val>>4) & 15];
  *p=   hex_ch[val & 15];
}

//
//	Do a text-mode file
//

void do_file_text(int fd) {
  int cnt;
  fd_set fset;
  char *bp;

  m_len= f_addr + n_byt;
  addr= 0;
  out_text_nl();

  if (f_real && fcntl(fd, F_SETFL, O_NONBLOCK))
    error("Can't set non-blocking input for real-time mode");

  while (0 != (cnt= read(fd, buf, buflen))) {
    if (cnt < 0) {
      if (errno != EAGAIN) error("Read error, errno==%d", errno);

      // We have to wait for more data - flush out the buffer if we
      // are in real-time mode.
      if (f_real) {
	char *p= op;
	out_flush(); op= obop= p;
      }
	
      FD_ZERO(&fset);
      FD_SET(fd, &fset);
      if (-1 == select(fd+1, &fset, 0, &fset, 0)) error("Select failure");
      continue;
    }

    // Process bulk of read here
    bp= buf;
    while (cnt-- > 0) {
      int ch= *bp++ & 255; addr++;
      char buf[4];
      int len= 1; buf[0]= ch;
      if (ch == '\\') { buf[1]= ch; len= 2; }
      else if (ch < 32 || ch >= 127) {
	buf[0]= '\\';
	if (ch < 32 && ctrl_ch[ch] != ' ') { buf[1]= ctrl_ch[ch]; len= 2; }
	else { buf[1]= 'x'; hex(buf+2, ch); len= 4; }
      }
      
      if (op + len - ocl > m_len) out_text_nl();
      memcpy(op, buf, len); op += len;
      if (ch == '\n') out_text_nl();
    }
  }
  
  if (f_addr) {
    if (op != ocl + f_addr)
      out_text_nl();
    op -= 2; *op++= '\n';
  }
  else if (op != ocl + f_addr) {
    *op++= '\n';
  }
  out_flush();
}

static inline void out_text_nl() {
  if (op != obuf) *op++= '\n';
  if (obuf_end - op <= m_len + 1)
    out_flush();

  ocl= op;
  if (f_addr) {
    hex(op, addr>>16);
    hex(op+2, addr>>8);
    hex(op+4, addr);
    op += 6; *op++= ':'; *op++= ' ';
  }
}


// END //

