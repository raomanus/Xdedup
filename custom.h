#ifndef _CUSTOM_H
#define _CUSTOM_H

#define NO_DEDUP 0x01
#define PARTIAL_DEDUP 0x02
#define DEBUG 0x04

struct arguments{
	int flag;
	char inputfile1[50];
	char inputfile2[50];
	char outputfile[50];
};

#endif
