/* assembler and programmer for rickp's MC5000 dev kit
 * pesco 2022, ISC license
 *
 * Reads MC5000 assembly code from stdin or a file. Translates the program and
 * writes it to the given MCU via serial port.
 *
 * See also: https://github.com/rickp/MC5000_DevKit
 */

#include <stdio.h>
#include <stdint.h>	/* uint8_t */
#include <stdlib.h>	/* exit */
#include <string.h>	/* strcmp, strdup */
#include <ctype.h>	/* isdigit */
#include <unistd.h>	/* getopt */
#include <termios.h>	/* tcgetattr, tcsetattr, cfsetspeed */
#include <regex.h>
#include <assert.h>
#include <err.h>

enum argtype {ARG_NONE, ARG_R, ARG_RI, ARG_L, ARG_P};
	
struct instr {
	const char *name;
	char code;
	enum argtype arg1, arg2;
};

struct instr instable[] = {
	/* basic instructions */
	{"nop", 1},
	{"mov", 2, ARG_RI, ARG_R},
	{"jmp", 3, ARG_L},
	{"slp", 4, ARG_RI},
	{"slx", 5, ARG_P},

	/* test instructions */
	{"teq", 6, ARG_RI, ARG_RI},
	{"tgt", 7, ARG_RI, ARG_RI},
	{"tlt", 8, ARG_RI, ARG_RI},
	{"tcp", 9, ARG_RI, ARG_RI},

	/* arithmetic instructions */
	{"add", 10, ARG_RI},
	{"sub", 11, ARG_RI},
	{"mul", 12, ARG_RI},
	{"not", 13},
	{"dgt", 14, ARG_RI},
	{"dst", 15, ARG_RI, ARG_RI},
};

/* pseudo-instruction */
#define OP_LBL 16

const char instr_regex[] =
    "^[ \t]*(([a-zA-Z][a-zA-Z0-9]*):)?"			/* label */
    "[ \t]*(([+-])?"					/* condition */
     "[ \t]*([a-zA-Z][a-zA-Z0-9]*)"			/* operation */
     "([ \t]+([a-zA-Z][a-zA-Z0-9]*|[+-]?[0-9]+))?"	/* argument 1 */
     "([ \t]+([a-zA-Z][a-zA-Z0-9]*|[+-]?[0-9]+))?)?"	/* argument 2 */
    "[ \t]*(#.*)?"					/* comment */
    "[ \t]*\n?$";					/* rest of line */

#define MATCH_LBL	2
#define MATCH_COND	4
#define MATCH_OPER	5
#define	MATCH_ARG1	7
#define MATCH_ARG2	9
#define NMATCH (MATCH_ARG2 + 1)

int
matchp(const regmatch_t *rm, int i)
{
	return (rm[i].rm_so != -1);
}

char *
matchstr(char *buf, const regmatch_t *rm, int i)
{
	if (!matchp(rm, i))
		return NULL;

	buf[rm[i].rm_eo] = '\0';	/* split the line up */
	return buf + rm[i].rm_so;
}

int
matchchr(const char *buf, const regmatch_t *rm, int i)
{
	if (!matchp(rm, i))
		return -1;

	return (unsigned char)buf[rm[i].rm_so];
}


char buf[1024];
FILE *outf = NULL;	/* output file */
FILE *devf = NULL;	/* serial port */
FILE *inf = NULL;	/* input file */
const char *fname;	/* input file name */
int mcu = 1;		/* programming target */
int line;		/* current line number */
int status;		/* exit code */
uint8_t checksum;

static const char *lbltab[256];
static const int maxlbl = sizeof lbltab / sizeof *lbltab;
static int nlabel = 0;

void emit_op(int, const char *, const char *, const char *);
void emit_op_lbl(const char *);
void emit_byte(int); // XXX

#define STR(X) matchstr(buf, rm, MATCH_ ## X)
#define CHR(X) matchchr(buf, rm, MATCH_ ## X)

void
usage(FILE *f, int x)
{
	extern const char *__progname;
	fprintf(f, "usage: %s [-u num] [-l dev | -o file] [file]\n",
	    __progname);
	exit(x);
}

int
main(int argc, char *argv[])
{
	regex_t re;
	regmatch_t rm[NMATCH];
	const char *op;
	int r, cond, label;

	/* compile the instruction regex */
	if ((r = regcomp(&re, instr_regex, REG_EXTENDED)) != 0) {
		regerror(r, &re, buf, sizeof buf);
		errx(1, "regcomp: %s", buf);
	}

	/* handle command line options */
	while ((r = getopt(argc, argv, "hl:o:u:")) != -1) {
		switch (r) {
		case 'h':
			usage(stdout, 0);
		case 'l':
			if ((devf = fopen(optarg, "r+")) == NULL)
				err(1, "%s", optarg);
			outf = devf;
			break;
		case 'o':
			if ((outf = fopen(optarg, "w")) == NULL)
				err(1, "%s", optarg);
			devf = NULL;
			break;
		case 'u':
			if (isdigit(*optarg) && optarg[1] == '\0')
				mcu = *optarg - '0';
			else
				err(1, "invalid argument '%s'. option -%c "
				    "expects a single digit MCU number",
				    optarg, r);
			break;
		default:
			usage(stderr, 1);
		}
	}
	argc -= optind;
	argv += optind;
	if (argc > 1)
		usage(stderr, 1);

	/* open input file */
	if (argc == 1) {
		fname = argv[0];
		if ((inf = fopen(fname, "r")) == NULL)
			err(1, "%s", fname);
	} else {
		fname = "(stdin)";
		inf = stdin;
	}

	/* start programming */
	if (devf != NULL) {
		struct termios t;

		/* set line speed */
		if (tcgetattr(fileno(devf), &t) != 0)
			err(1, "tcgetattr");
		cfsetspeed(&t, B19200);
		if (tcsetattr(fileno(devf), TCSAFLUSH, &t) != 0)
			err(1, "tcsetattr");

		emit_byte(0x7F);		/* start code */
		emit_byte(0x30 + mcu);	/* chip id */
	}

	/* process input */
	for (line = 1, label = 0; !feof(inf); line++) {
		/* read one line of input */
		buf[sizeof buf - 2] = '\0';
		if (fgets(buf, sizeof buf, inf) == NULL)
			break;
		if (buf[sizeof buf - 2] != '\0' &&
		    buf[sizeof buf - 2] != '\n') {
			fprintf(stderr, "%s:%d: overlong line\n", fname, line);
			status = 1;
			continue;
		}

		/* parse the line */
		r = regexec(&re, buf, NMATCH, rm, 0);
		if (r == REG_NOMATCH) {
			fprintf(stderr, "%s:%d: syntax error\n", fname, line);
			status = 1;
			continue;
		}
		else if (r != 0) {
			regerror(r, &re, buf, sizeof buf);
			errx(1, "regexec: %s (line %d)", buf, line);
		}

		/* emit a pseudo-operation for labels */
		emit_op_lbl(STR(LBL));

		/* emit the actual instruction, if present */
		emit_op(CHR(COND), STR(OPER), STR(ARG1), STR(ARG2));
	}
	if (ferror(inf))
		errx(1, "%s:%d: read error", fname, line);

	/* finish programming */
	if (devf != NULL) {
		uint8_t resp[3];

		checksum = -checksum >> 2;
		emit_byte(checksum);
		emit_byte(0x7E);		/* end code */
		fflush(devf);

		/* read and validate response */
		if (fread(resp, 1, 3, devf) < 3)
			errx(1, "error reading response from board");
		if (resp[0] != 0x7F)
			errx(1, "unexpected response received");
		if (resp[1] != 0x30 + mcu)
			errx(1, "wrong chip ID (0x%.2x) in response", resp[1]);
		if (resp[2] == 1)
			printf("MCU #%d: program accepted\n", mcu);
		else if (resp[2] == 0)
			errx(1, "MCU #%d: programming failure", mcu);
		else
			errx(1, "MCU #%d: unknown result (%d)", mcu, resp[2]);
	}

	return status;
}

void
emit_byte(int n)
{
	assert(n >= 0);
	assert(n < 256);
	if (outf != NULL)
		fputc(n, outf);
	checksum += n;
}

void
emit_dummy(void)
{
	emit_byte(0xFF);
	status = 1;
}

void
emit_xbus(const char *arg)
{
	if (arg[0] != 'x') {
		fprintf(stderr, "%s:%d: %s is not an XBus port\n",
		    fname, line, arg);
		emit_dummy();
		return;
	}

	// XXX ?
	if (arg[1] == '0')		/* x0 */
		emit_byte(0x40);	/* = 01000000 */
	else if (arg[1] == '1')		/* x1 */
		emit_byte(0x00);	/* = 00000000 */
	else {
		fprintf(stderr, "%s:%d: undefined port %s\n", fname, line,
		    arg);
		emit_dummy();
	}
}

int
emit_reg(const char *arg)
{
	static struct {const char *name; uint8_t val;} regtable[] = {
		{"acc",	0x70},	/* 01110000 */
		{"dat",	0x60},	/* 01100000 */
		{"p0",	0x50},	/* 01010000 */
		{"p1",	0x58},	/* 01011000 */
		{"x0",	0x40},	/* 01000000 */
		{"x1",	0x48},	/* 01001000 */
	};
	static const int n = sizeof regtable / sizeof *regtable;
	int i;

	assert(arg != NULL);
	if (arg[0] == '+' || arg[0] == '-' || isdigit(arg[0]))
		return 0;		/* it's a number */

	for (i = 0; i < n; i++)
		if (strcmp(arg, regtable[i].name) == 0)
			break;
	if (i == n) {
		fprintf(stderr, "%s:%d: undefined register %s\n", fname, line,
		    arg);
		emit_dummy();
	}

	emit_byte(regtable[i].val);
	return 1;
}

void
emit_int(const char *arg)
{
	int i;

	assert(arg != NULL);
	i = atoi(arg);
	if (i > 999)
		i = 999;
	else if (i < -999)
		i = -999;
	i += 1000;

	/* 000hhhhh 00llllll */
	assert(i >= 0);
	assert(i < 2000);
	emit_byte(i >> 6);		/* 5 high bits */
	emit_byte(i & 0x3F);		/* 6 low bits */
}

int
find_label(const char *arg)
{
	int i;

	/* look for the given label in the table */
	for (i = 0; i < nlabel; i++)
		if (strcmp(lbltab[i], arg) == 0)
			return i;

	/* if not found, assign next free number */
	assert(nlabel < maxlbl);
	if ((lbltab[nlabel] = strdup(arg)) == NULL)
		err(1, "new_label");

	return nlabel++;
}

void
emit_label(const char *arg)
{
	emit_byte(find_label(arg));
}

void
emit_arg(const char *op, enum argtype ty, const char *arg)
{
	if (ty == ARG_NONE) {
		if (arg != NULL) {
			fprintf(stderr, "%s:%d: extra argument to %s: %s\n",
			    fname, line, op, arg);
			status = 1;
		}
		return;
	}

	if (arg == NULL) {
		fprintf(stderr, "%s:%d: %s missing argument\n", fname, line,
		    op);
		emit_dummy();
		return;
	}

	switch(ty) {
	case ARG_R:
		if (!emit_reg(arg)) {
			fprintf(stderr, "%s:%d: register expected, not %s\n",
			    fname, line, arg);
			emit_dummy();
		}
		break;
	case ARG_RI:
		if (emit_reg(arg))		/* 1 byte */
			emit_byte(0x00);	/* 1 byte */
		else
			emit_int(arg);		/* 2 bytes */
		break;
	case ARG_L:
		emit_label(arg);
		break;
	case ARG_P:
		emit_xbus(arg);
		break;
	default:
		errx(1, "undefined argtype %d\n", ty);
	}
}

void
emit_op(int cond, const char *op, const char *arg1, const char *arg2)
{
	static const int n = sizeof instable / sizeof *instable;
	int i;
	uint8_t b;

	if (op == NULL)
		return;

	/* find the operation in the table */
	for (i = 0; i < n; i++)
		if (strcmp(instable[i].name, op) == 0)
			break;
	if (i == n) {
		fprintf(stderr, "%s:%d: undefined instruction %s\n", fname,
		    line, op);
		emit_dummy();
		return;
	}

	/* encode and emit the operation byte: 0ccccccmp */
	b = instable[i].code << 2;
	if (cond != -1) {
		if (cond == '+')
			b |= 0x01;
		else if (cond == '-')
			b |= 0x02;
		else
			errx(1, "undefined flag %c (line %d)", cond, line);
	}
	emit_byte(b);

	/* emit the arguments */
	emit_arg(op, instable[i].arg1, arg1);
	emit_arg(op, instable[i].arg2, arg2);
}

void
emit_op_lbl(const char *arg)
{
	if (arg == NULL)
		return;

	emit_byte(OP_LBL << 2);
	emit_byte(find_label(arg));
}
