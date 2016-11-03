#include "data.table.h"
#include "fwriteLookups.h"
#include <errno.h>
#include <unistd.h>  // for access()
#include <fcntl.h>
#include <time.h>
#ifdef WIN32
#include <sys/types.h>
#include <sys/stat.h>
#include <io.h>
#define WRITE _write
#define CLOSE _close
#else
#define WRITE write
#define CLOSE close
#endif

#define NUM_SF   15
#define SIZE_SF  1000000000000000ULL  // 10^NUM_SF

// Globals for this file only (written once to hold parameters passed from R level)                   
static const char *na_str;             // by default "" or if set then usually "NA"
static size_t na_len;                  // nchar(na_str). So 0 for "", or 2 for "NA"
static char col_sep;                   // comma in .csv files
static char dec_sep;                   // the '.' in the number 3.1416. In Europe often: 3,1416
static Rboolean verbose=FALSE;         // be chatty?
static Rboolean quote=FALSE;           // whether to surround fields with double quote ". NA means 'auto' (default)
static Rboolean qmethod_escape=TRUE;   // when quoting fields, how to manage double quote in the field contents

static inline void writeInteger(long long x, char **thisCh)
{
  char *ch = *thisCh;
  // both integer and integer64 are passed to this function so careful
  // to test for NA_INTEGER in the calling code. INT_MIN (NA_INTEGER) is
  // a valid non-NA in integer64
  if (x == 0) {
    *ch++ = '0';
  } else {
    if (x<0) { *ch++ = '-'; x=-x; }
    // avoid log() call for speed. write backwards then reverse when we know how long
    int width = 0;
    while (x>0) { *ch++ = '0'+x%10; x /= 10; width++; }
    for (int i=width/2; i>0; i--) {
      char tmp=*(ch-i);
      *(ch-i) = *(ch-width+i-1);
      *(ch-width+i-1) = tmp;
    }
  }
  *thisCh = ch;
}

SEXP genLookups() {
  Rprintf("genLookups commented out of the package so it's clear it isn't needed to build. The hooks are left in so it's easy to put back in development should we need to.\n");
  // e.g. ldexpl may not be available on some platforms, or if it is it may not be accurate.
  return R_NilValue;
}
/*
  FILE *f = fopen("/tmp/fwriteLookups.h", "w"); 
  fprintf(f, "//\n\
// Generated by fwrite.c:genLookups()\n\
//\n\
// 3 vectors: sigparts, expsig and exppow\n\
// Includes precision higher than double; leave this compiler on this machine\n\
// to parse the literals at reduced precision.\n\
// 2^(-1023:1024) is held more accurately than double provides by storing its\n\
// exponent separately (expsig and exppow)\n\
// We don't want to depend on 'long double' (>64bit) availability to generate\n\
// these at runtime; libraries and hardware vary.\n\
// These small lookup tables are used for speed.\n\
//\n\n");
  fprintf(f, "double sigparts[53] = {\n0.0,\n");
  for (int i=1; i<=52; i++) {
    fprintf(f, "%.40Le%s\n",ldexpl(1.0L,-i), i==52?"":",");
  }
  fprintf(f, "};\n\ndouble expsig[2048] = {\n");
  char x[2048][60];
  for (int i=0; i<2048; i++) {
    sprintf(x[i], "%.40Le", ldexpl(1.0L, i-1023));
    fprintf(f, "%.*s%s\n", (int)(strchr(x[i],'e')-x[i]), x[i], (i==2047?"":",") );
  }
  fprintf(f, "};\n\nint exppow[2048] = {\n");
  for (int i=0; i<2048; i++) {
    fprintf(f, "%d%s", atoi(strchr(x[i],'e')+1), (i==2047?"":",") );
  }
  fprintf(f, "};\n\n");
  fclose(f);
  return R_NilValue;
}
*/

static union {
  double d;
  unsigned long long ull;
} u;

static inline void writeNumeric(double x, char **thisCh)
{
  // hand-rolled / specialized for speed
  // *thisCh is safely the output destination with enough space (ensured via calculating maxLineLen up front)
  // technique similar to base R (format.c:formatReal and printutils.c:EncodeReal0)
  // differences/tricks :
  //   i) no buffers. writes straight to the final file buffer passed to write()
  //  ii) no C libary calls such as sprintf() where the fmt string has to be interpretted over and over
  // iii) no need to return variables or flags.  Just writes.
  //  iv) shorter, easier to read and reason with. In one self contained place.
  char *ch = *thisCh;
  if (!R_FINITE(x)) {
    if (ISNAN(x)) {
      memcpy(ch, na_str, na_len); ch += na_len; // by default na_len==0 and the memcpy call will be skipped
    } else if (x>0) {
      *ch++ = 'I'; *ch++ = 'n'; *ch++ = 'f';
    } else {
      *ch++ = '-'; *ch++ = 'I'; *ch++ = 'n'; *ch++ = 'f';
    }
  } else if (x == 0.0) {
    *ch++ = '0';   // and we're done.  so much easier rather than passing back special cases
  } else {
    if (x < 0.0) { *ch++ = '-'; x = -x; }  // and we're done on sign, already written. no need to pass back sign
    u.d = x;
    unsigned long long fraction = u.ull & 0xFFFFFFFFFFFFF;  // (1ULL<<52)-1;
    int exponent = (int)((u.ull>>52) & 0x7FF);              // [0,2047]

    // Now sum the appropriate powers 2^-(1:52) of the fraction 
    // Important for accuracy to start with the smallest first; i.e. 2^-52
    // Exact powers of 2 (1.0, 2.0, 4.0, etc) are represented precisely with fraction==0
    // Skip over tailing zeros for exactly representable numbers such 0.5, 0.75
    // Underflow here (0u-1u = all 1s) is on an unsigned type which is ok by C standards
    // sigparts[0] arranged to be 0.0 in genLookups() to enable branch free loop here
    double acc = 0;  // 'long double' not needed
    int i = 52;
    if (fraction) {
      while ((fraction & 0xFF) == 0) { fraction >>= 8; i-=8; } 
      while (fraction) {
        acc += sigparts[(((fraction&1u)^1u)-1u) & i];
        i--;
        fraction >>= 1;
      }
    }
    // 1.0+acc is in range [1.5,2.0) by IEEE754
    // expsig is in range [1.0,10.0) by design of fwriteLookups.h
    // Therefore y in range [1.5,20.0)
    // Avoids (potentially inaccurate and potentially slow) log10/log10l, pow/powl, ldexp/ldexpl
    // By design we can just lookup the power from the tables
    double y = (1.0+acc) * expsig[exponent];  // low magnitude mult
    int exp = exppow[exponent];
    if (y>=10.0) { y /= 10; exp++; }
    unsigned long long l = y * SIZE_SF;  // low magnitude mult 10^NUM_SF
    // l now contains NUM_SF+1 digits as integer where repeated /10 below is accurate

    // if (verbose) Rprintf("\nTRACE: acc=%.20Le ; y=%.20Le ; l=%llu ; e=%d     ", acc, y, l, exp);    

    if (l%10 >= 5) l+=10; // use the last digit to round
    l /= 10;
    if (l == 0) {
      if (*(ch-1)=='-') ch--;
      *ch++ = '0';
    } else {
      // Count trailing zeros and therefore s.f. present in l
      int trailZero = 0;
      while (l%10 == 0) { l /= 10; trailZero++; }
      int sf = NUM_SF - trailZero;
      if (sf==0) {sf=1; exp++;}  // e.g. l was 9999999[5-9] rounded to 10000000 which added 1 digit
      
      // l is now an unsigned long that doesn't start or end with 0
      // sf is the number of digits now in l
      // exp is e<exp> were l to be written with the decimal sep after the first digit
      int dr = sf-exp-1; // how many characters to print to the right of the decimal place
      int width=0;       // field width were it written decimal format. Used to decide whether to or not.
      int dl0=0;         // how many 0's to add to the left of the decimal place before starting l
      if (dr<=0) { dl0=-dr; dr=0; width=sf+dl0; }  // 1, 10, 100, 99000
      else {
        if (sf>dr) width=sf+1;                     // 1.234 and 123.4
        else { dl0=1; width=dr+1+dl0; }            // 0.1234, 0.0001234
      }
      // So:  3.1416 => l=31416, sf=5, exp=0     dr=4; dl0=0; width=6
      //      30460  => l=3046, sf=4, exp=4      dr=0; dl0=1; width=5
      //      0.0072 => l=72, sf=2, exp=-3       dr=4; dl0=1; width=6
      if (width <= sf + (sf>1) + 2 + (abs(exp)>99?3:2)) {
         //              ^^^^ to not include 1 char for dec in -7e-04 where sf==1
         //                      ^ 2 for 'e+'/'e-'
         // decimal format ...
         ch += width-1;
         if (dr) {
           while (dr && sf) { *ch--='0'+l%10; l/=10; dr--; sf--; }
           while (dr) { *ch--='0'; dr--; }
           *ch-- = dec_sep;
         }
         while (dl0) { *ch--='0'; dl0--; }
         while (sf) { *ch--='0'+l%10; l/=10; sf--; }
         // ch is now 1 before the first char of the field so position it afterward again, and done
         ch += width+1;
      } else {
        // scientific ...
        ch += sf;  // sf-1 + 1 for dec
        for (int i=sf; i>1; i--) {
          *ch-- = '0' + l%10;   
          l /= 10;
        }
        if (sf == 1) ch--; else *ch-- = dec_sep;
        *ch = '0' + l;
        ch += sf + (sf>1);
        *ch++ = 'e';  // lower case e to match base::write.csv
        if (exp < 0) { *ch++ = '-'; exp=-exp; }
        else { *ch++ = '+'; }  // to match base::write.csv
        if (exp < 100) {
          *ch++ = '0' + (exp / 10);
          *ch++ = '0' + (exp % 10);
        } else {
          *ch++ = '0' + (exp / 100);
          *ch++ = '0' + (exp / 10) % 10;
          *ch++ = '0' + (exp % 10);
        }
      }
    }
  }
  *thisCh = ch;
}

static inline int maxStrLen(SEXP x) {
  // The max nchar of any string in a column or factor level
  int max=na_len, nrow=length(x);
  for (int i=0; i<nrow; i++) {
    int l = LENGTH(STRING_ELT(x,i));  // just looks at header. no need for strlen scan
    if (l>max) max=l;
  }
  return max*2 +2;
  //        ^^ every character in field could be a double quote, each to be escaped (!)
  //           ^^ surround quotes counted always for safety e.g. every field could contain a \n
}

static inline void writeString(SEXP x, char **thisCh)
{
  char *ch = *thisCh;
  if (x == NA_STRING) {
    // NA is not quoted by write.csv even when quote=TRUE to distinguish from "NA"
    memcpy(ch, na_str, na_len); ch += na_len;
  } else {
    Rboolean q = quote;
    if (q==NA_LOGICAL) { // quote="auto"
      const char *tt = CHAR(x);
      while (*tt!='\0' && *tt!=col_sep && *tt!='\n') *ch++ = *tt++;
      // windows includes \n in its \r\n so looking for \n only is sufficient
      if (*tt=='\0') {
        // most common case: no sep or newline contained in string
        *thisCh = ch;  // advance caller over the field already written
        return;
      }
      ch = *thisCh; // rewind the field written since it contains some sep or \n
      q = TRUE;
    }
    if (q==FALSE) {
      memcpy(ch, CHAR(x), LENGTH(x));
      ch += LENGTH(x);
    } else {
      *ch++ = '"';
      const char *tt = CHAR(x);
      if (qmethod_escape) {
        while (*tt!='\0') {
          if (*tt=='"' || *tt=='\\') *ch++ = '\\';
          *ch++ = *tt++;
        }
      } else {
        // qmethod='double'
        while (*tt!='\0') {
          if (*tt=='"') *ch++ = '"';
          *ch++ = *tt++;
        }
      }
      *ch++ = '"';
    }
  }
  *thisCh = ch;
}       

inline Rboolean isInteger64(SEXP x) {
  SEXP class = getAttrib(x, R_ClassSymbol);
  if (isString(class)) {
    for (int i=0; i<LENGTH(class); i++) {   // inherits()
      if (STRING_ELT(class, i) == char_integer64) return TRUE;
    }
  }
  return FALSE;
}

SEXP writefile(SEXP list_of_columns,
               SEXP filenameArg,
               SEXP col_sep_Arg,
               SEXP row_sep_Arg,
               SEXP na_Arg,
               SEXP dec_Arg,
               SEXP quoteArg,           // 'auto'=NA_LOGICAL|TRUE|FALSE
               SEXP qmethod_escapeArg,  // TRUE|FALSE
               SEXP append,             // TRUE|FALSE
               SEXP row_names,          // TRUE|FALSE
               SEXP col_names,          // TRUE|FALSE
               SEXP showProgressArg,
               SEXP verboseArg,
               SEXP turboArg)
{
  if (!isNewList(list_of_columns)) error("fwrite must be passed an object of type list, data.table or data.frame");
  RLEN ncols = length(list_of_columns);
  if (ncols==0) error("fwrite must be passed a non-empty list");
  RLEN nrows = length(VECTOR_ELT(list_of_columns, 0));
  for (int i=1; i<ncols; i++) {
    if (nrows != length(VECTOR_ELT(list_of_columns, i)))
      error("Column %d's length (%d) is not the same as column 1's length (%d)", i+1, length(VECTOR_ELT(list_of_columns, i)), nrows);
  }
#ifndef _OPENMP
  Rprintf("Your platform/environment has not detected OpenMP support. fwrite() will still work, but slower in single threaded mode.\n");
  // Rprintf rather than warning() because warning() would cause test.data.table() to error about the unexpected warnings
#endif

  const Rboolean showProgress = LOGICAL(showProgressArg)[0];
  time_t start = time(NULL);
  time_t nexttime = start+2; // start printing progress meter in 2 sec if not completed by then
  
  verbose = LOGICAL(verboseArg)[0];
  const Rboolean turbo = LOGICAL(turboArg)[0];
  
  col_sep = *CHAR(STRING_ELT(col_sep_Arg, 0));  // DO NOT DO: allow multichar separator (bad idea)
  const char *row_sep = CHAR(STRING_ELT(row_sep_Arg, 0));
  int row_sep_len = strlen(row_sep);  // someone somewhere might want a trailer on every line
  na_str = CHAR(STRING_ELT(na_Arg, 0));
  na_len = strlen(na_str);
  dec_sep = *CHAR(STRING_ELT(dec_Arg,0));
  quote = LOGICAL(quoteArg)[0];
  qmethod_escape = LOGICAL(qmethod_escapeArg)[0];
  const char *filename = CHAR(STRING_ELT(filenameArg, 0));

  errno = 0;   // clear flag possibly set by previous errors
  int f;
  if (*filename=='\0') {
    f=-1;  // file="" means write to standard output
    row_sep = "\n";  // We'll use Rprintf(); it knows itself about \r\n on Windows
    row_sep_len = 1;
  } else { 
#ifdef WIN32
    f = _open(filename, _O_WRONLY | _O_BINARY | _O_CREAT | (LOGICAL(append)[0] ? _O_APPEND : _O_TRUNC), _S_IWRITE);
    // row_sep must be passed from R level as '\r\n' on Windows since write() only auto-converts \n to \r\n in
    // _O_TEXT mode. We use O_BINARY for full control and perhaps speed since O_TEXT must have to deep branch an if('\n')
#else
    f = open(filename, O_WRONLY | O_CREAT | (LOGICAL(append)[0] ? O_APPEND : O_TRUNC), 0644);
#endif
    if (f == -1) {
      char *err = strerror(errno);
      if( access( filename, F_OK ) != -1 )
        error("'%s'. Failed to open existing file for writing. Do you have write permission to it? Is this Windows and does another process such as Excel have it open? File: %s", err, filename);
      else
        error("'%s'. Unable to create new file for writing (it does not exist already). Do you have permission to write here and is there space on the disk? File: %s", err, filename); 
    }
  }
  int true_false;
  
  clock_t t0=clock();
  // i) prefetch levels of factor columns (if any) to save getAttrib on every field on every row of any factor column
  // ii) calculate certain upper bound of line length
  SEXP levels[ncols];  // on-stack vla
  int lineLenMax = 2;  // initialize with eol max width of \r\n on windows
  int sameType = TYPEOF(VECTOR_ELT(list_of_columns, 0));
  Rboolean integer64[ncols]; // store result of isInteger64() per column for efficiency
  SEXP rn = NULL;
  if (LOGICAL(row_names)[0]) {
    rn = getAttrib(list_of_columns, R_RowNamesSymbol);
    if (isString(rn)) {
      // for data.frame; data.table never has row.names
      lineLenMax += maxStrLen(rn) +1/*first col_sep*/;
    } else {
      // implicit row.names
      rn = NULL;
      lineLenMax += (int)log10(nrows) +1 +2/*surrounding quotes if quote=TRUE*/ +1/*first col_sep*/;
    }
  }
  for (int col_i=0; col_i<ncols; col_i++) {
    SEXP column = VECTOR_ELT(list_of_columns, col_i);
    integer64[col_i] = FALSE;
    switch(TYPEOF(column)) {
    case LGLSXP:
      lineLenMax+=5;  // width of FALSE
      break;
    case REALSXP:
      integer64[col_i] = isInteger64(column);
      lineLenMax+=25;   // +- 15digits dec e +- nnn = 22 + 3 safety = 25. That covers int64 too (20 digits).
      break;
    case INTSXP:
      if (isFactor(column)) {
        levels[col_i] = getAttrib(column, R_LevelsSymbol);
        sameType = 0; // TODO: enable deep-switch-avoidance for all columns factor
        lineLenMax += maxStrLen(levels[col_i]); 
      } else {
        levels[col_i] = NULL;
        lineLenMax+=11;   // 11 + sign
      }
      break;
    case STRSXP:
      lineLenMax += maxStrLen(column);
      break;
    default:
      error("Column %d's type is '%s' - not yet implemented.", col_i+1,type2char(TYPEOF(column)) );
    }
    if (TYPEOF(column) != sameType || integer64[col_i]) sameType = 0;
    // we could code up all-integer64 case below as well but that seems even less
    // likely in practice than all-int or all-double
    lineLenMax++;  // column separator
  }
  clock_t tlineLenMax=clock()-t0;
  if (verbose) Rprintf("Maximum line length is %d calculated in %.3fs\n", lineLenMax, 1.0*tlineLenMax/CLOCKS_PER_SEC);
  // TODO: could parallelize by column, but currently no need as insignificant time
  t0=clock();

  if (verbose) Rprintf("Writing column names ... ");
  if (LOGICAL(col_names)[0]) {
    SEXP names = getAttrib(list_of_columns, R_NamesSymbol);  
    if (names!=NULL) {
      if (LENGTH(names) != ncols) error("Internal error: length of column names is not equal to the number of columns. Please report.");
      int bufSize = 1;  // if row.names then 1 for first col_sep
      for (int col_i=0; col_i<ncols; col_i++) bufSize += LENGTH(STRING_ELT(names, col_i));
      bufSize *= 2;  // in case every column name is filled with quotes to be escaped (!)
      bufSize += ncols*(2/*beginning and ending quote*/ + 1/*sep*/) + 2/*line ending (\r\n on windows)*/ + 1/*\0*/;
      char *buffer = malloc(bufSize);
      if (buffer == NULL) error("Unable to allocate %dMB buffer for column names", bufSize/(1024*1024));
      char *ch = buffer;
      if (LOGICAL(row_names)[0]) {
        if (quote!=FALSE) { *ch++='"'; *ch++='"'; } // to match write.csv
        *ch++ = col_sep;
      }
      for (int col_i=0; col_i<ncols; col_i++) {
        writeString(STRING_ELT(names, col_i), &ch);
        *ch++ = col_sep;
      }
      ch--;  // backup onto the last col_sep after the last column
      memcpy(ch, row_sep, row_sep_len);  // replace it with the newline 
      ch += row_sep_len;
      if (f==-1) { *ch='\0'; Rprintf(buffer); }
      else if (WRITE(f, buffer, (int)(ch-buffer))==-1) { close(f); error("Error writing to file: %s", filename); }
      free(buffer);
    }
  }
  if (verbose) Rprintf("done in %.3fs\n", 1.0*(clock()-t0)/CLOCKS_PER_SEC);
  if (nrows == 0) {
    if (verbose) Rprintf("No data rows present (nrow==0)\n");
    if (f!=-1 && CLOSE(f)) error("Error closing file: %s", filename);
    return(R_NilValue);
  }

  // Decide buffer size on each core
  // Large enough to fit many lines (to reduce calls to write()). Small enough to fit in each core's cache.
  // If the lines turn out smaller, that's ok. we just won't use all the buffer in that case. But we must be
  // sure to allow for worst case; i.e. every row in the batch all being the maximum line length.
  int bufSize = 1*1024*1024;  // 1MB  TODO: experiment / fetch cache size
  if (lineLenMax > bufSize) bufSize = lineLenMax;
  const int rowsPerBatch = bufSize/lineLenMax;
  const int numBatches = (nrows-1)/rowsPerBatch + 1;
  if (verbose) Rprintf("Writing data rows in %d batches of %d rows (each buffer size %.3fMB, turbo=%d, showProgress=%d) ... ",
    numBatches, rowsPerBatch, 1.0*bufSize/(1024*1024), turbo, showProgress);
  t0 = clock();
  
  int nth;
  Rboolean failed=FALSE, hasPrinted=FALSE;
  int failed_reason=0;  // -1 for malloc fail, else write's errno (>=1)
  #pragma omp parallel num_threads(getDTthreads())
  {
    char *ch, *buffer;              // local to each thread
    ch = buffer = malloc(bufSize);  // each thread has its own buffer
    // Don't use any R API alloc here (e.g. R_alloc); they are
    // not thread-safe as per last sentence of R-exts 6.1.1. 
    
    if (buffer==NULL) {failed=TRUE; failed_reason=-1;}
    // Do not rely on availability of '#omp cancel' new in OpenMP v4.0 (July 2013).
    // OpenMP v4.0 is in gcc 4.9+ (https://gcc.gnu.org/wiki/openmp) but
    // not yet in clang as of v3.8 (http://openmp.llvm.org/)
    // If not-me failed, I'll see shared 'failed', fall through loop, free my buffer
    // and after parallel section, single thread will call R API error() safely.
    
    #pragma omp single
    {
      nth = omp_get_num_threads();
    }
    int me = omp_get_thread_num();
    
    #pragma omp for ordered schedule(dynamic)
    for(RLEN start_row = 0; start_row < nrows; start_row += rowsPerBatch) {
      if (failed) continue;  // Not break. See comments above about #omp cancel
      int upp = start_row + rowsPerBatch;
      if (upp > nrows) upp = nrows;
      if (turbo && sameType==REALSXP && !LOGICAL(row_names)[0]) {
        // avoid deep switch() on type. turbo switches on both sameType and specialized writeNumeric
        for (RLEN row_i = start_row; row_i < upp; row_i++) {
          for (int col_i = 0; col_i < ncols; col_i++) {
            SEXP column = VECTOR_ELT(list_of_columns, col_i);
            writeNumeric(REAL(column)[row_i], &ch);
            *ch++ = col_sep;
          }
          ch--;  // backup onto the last col_sep after the last column
          memcpy(ch, row_sep, row_sep_len);  // replace it with the newline.
          ch += row_sep_len;
        }
      } else if (turbo && sameType==INTSXP && !LOGICAL(row_names)[0]) {
        for (RLEN row_i = start_row; row_i < upp; row_i++) {
          for (int col_i = 0; col_i < ncols; col_i++) {
            SEXP column = VECTOR_ELT(list_of_columns, col_i);
            if (INTEGER(column)[row_i] == NA_INTEGER) {
              memcpy(ch, na_str, na_len); ch += na_len;
            } else {
              writeInteger(INTEGER(column)[row_i], &ch);
            }
            *ch++ = col_sep;
          }
          ch--;
          memcpy(ch, row_sep, row_sep_len);
          ch += row_sep_len;
        }
      } else {
        // mixed types. switch() on every cell value since must write row-by-row
        for (RLEN row_i = start_row; row_i < upp; row_i++) {
          if (LOGICAL(row_names)[0]) {
            if (rn==NULL) {
              if (quote!=FALSE) *ch++='"';  // default 'auto' will quote the row.name numbers
              writeInteger(row_i+1, &ch);
              if (quote!=FALSE) *ch++='"';
            } else {
              writeString(STRING_ELT(rn, row_i), &ch);
            }
            *ch++=col_sep;
          }
          for (int col_i = 0; col_i < ncols; col_i++) {
            SEXP column = VECTOR_ELT(list_of_columns, col_i);
            switch(TYPEOF(column)) {
            case LGLSXP:
              true_false = LOGICAL(column)[row_i];
              if (true_false == NA_LOGICAL) {
                memcpy(ch, na_str, na_len); ch += na_len;
              } else if (true_false) {
                memcpy(ch,"TRUE",4);   // Other than strings, field widths are limited which we check elsewhere here to ensure
                ch += 4;
              } else {
                memcpy(ch,"FALSE",5);
                ch += 5;
              }
              break;
            case REALSXP:
              if (integer64[col_i]) {
                long long i64 = *(long long *)&REAL(column)[row_i];
                if (i64 == NAINT64) {
                  memcpy(ch, na_str, na_len); ch += na_len;
                } else {
                  if (turbo) {
                    writeInteger(i64, &ch);
                  } else {
                    ch += sprintf(ch,
                    #ifdef WIN32
                        "%I64d"
                    #else
                        "%lld"
                    #endif
                    , i64);
                  }
                }
              } else {
                if (turbo) {
                  writeNumeric(REAL(column)[row_i], &ch); // handles NA, Inf etc within it
                } else {
                  // if there are any problems with the specialized writeNumeric, user can revert to (slower) standard library
                  if (ISNAN(REAL(column)[row_i])) {
                    memcpy(ch, na_str, na_len); ch += na_len;
                  } else {
                    ch += sprintf(ch, "%.15g", REAL(column)[row_i]);
                  }
                }
              }
              break;
            case INTSXP:
              if (INTEGER(column)[row_i] == NA_INTEGER) {
                memcpy(ch, na_str, na_len); ch += na_len;
              } else if (levels[col_i] != NULL) {   // isFactor(column) == TRUE
                writeString(STRING_ELT(levels[col_i], INTEGER(column)[row_i]-1), &ch);
              } else {
                if (turbo) {
                  writeInteger(INTEGER(column)[row_i], &ch);
                } else {
                  ch += sprintf(ch, "%d", INTEGER(column)[row_i]);
                }
              }
              break;
            case STRSXP:
              writeString(STRING_ELT(column, row_i), &ch);
              break;
            // default:
            // An uncovered type would have already thrown above when calculating maxLineLen earlier
            }
            *ch++ = col_sep;
          }
          ch--;  // backup onto the last col_sep after the last column
          memcpy(ch, row_sep, row_sep_len);  // replace it with the newline. TODO: replace memcpy call with eol1 eol2 --eolLen 
          ch += row_sep_len;
        }
      }
      #pragma omp ordered
      {
        if (f==-1) {
          *ch='\0';  // standard C string end marker so Rprintf knows where to stop
          Rprintf(buffer);
          // nth==1 at this point since when file=="" (f==-1 here) fwrite.R calls setDTthreads(1)
          // Although this ordered section is one-at-a-time it seems that calling Rprintf() here, even with a
          // R_FlushConsole() too, causes corruptions on Windows but not on Linux. At least, as observed so
          // far using capture.output(). Perhaps Rprintf() updates some state or allocation that cannot be done
          // by slave threads, even when one-at-a-time. Anyway, made this single-threaded when output to console
          // to be safe (setDTthreads(1) in fwrite.R) since output to console doesn't need to be fast.
        } else {
          if (!failed && WRITE(f, buffer, (int)(ch-buffer)) == -1) {
            failed=TRUE; failed_reason=errno;
          }
          // The !failed is so the other threads that were waiting at this '#omp ordered' don't try
          // and write after the first fail.
          
          time_t now;
          if (me==0 && showProgress && (now=time(NULL))>=nexttime) {
            // See comments above inside the f==-1 clause.
            // Not only is this ordered section one-at-a-time but we'll also Rprintf() here only from the
            // master thread (me==0) and hopefully this will work on Windows. If not, user should set
            // showProgress=FALSE until this can be fixed or removed.
            int eta = (int)((nrows-upp)*(((double)(now-start))/upp));
            if (hasPrinted || eta >= 2) {
              Rprintf("\rWritten %.1f%% of %d rows in %d secs using %d thread%s. ETA %d secs.",
                       (100.0*upp)/nrows, nrows, (int)(now-start), nth, nth==1?"":"s", eta);
              R_FlushConsole();    // for Windows
              nexttime = now+1;
              hasPrinted = TRUE;
            }
          }
          // TODO: Adding a progress bar here with Rprintf() or similar should be possible in theory, but see
          // the comment above about corruptions observed on windows. Could try and see. If the progress bar
          // corruputed then there's not too much harm. It could be delayed and flushed, or, best just have
          // one thread handle its update.
        }
        ch = buffer;
      }
    }
    free(buffer);
    // all threads will call this free on their buffer, even if one or more threads had malloc fail.
    // If malloc() failed for me, free(NULL) is ok and does nothing.
  }
  // Finished parallel region and can call R API safely now.
  if (hasPrinted) {
    // clear the progress meter
    Rprintf("\r                                                                                   \r");
    R_FlushConsole();  // for Windows
  }
  if (failed && failed_reason==-1)
    error("One or more threads failed to alloc or realloc their private buffer. Out of memory.\n");
  if (f!=-1 && CLOSE(f) && !failed) error("Error closing file '%s': %s", filename, strerror(errno));
  // quoted '%s' in case of trailing spaces in the filename
  // If a write failed, the line above tries close() but that might fail as well. The && !failed is to not
  // report the error as just 'closing file' but the next line for more detail from the original error on write.
  if (failed) error("Failed write to '%s': %s. Out of disk space is most likely especially if /dev/shm or /tmp since they have smaller limits, or perhaps network issue if NFS. Your operating system reported that it opened the file ok in write mode but perhaps it only checks permissions when actually writing some data.", filename, strerror(failed_reason));
  if (verbose) Rprintf("all %d threads done\n", nth);  // TO DO: report elapsed time since (clock()-t0)/NTH is only estimate
  return(R_NilValue);  // must always return SEXP from C level otherwise hang on Windows
}


