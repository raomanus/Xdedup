#include <asm/unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <ctype.h>
#include "custom.h"

#ifndef __NR_xdedup
#error xdedup system call not defined
#endif

int main(int argc, char **argv)
{
	int rc = 0;
	int c;
	int index;
	int i;
	int j;
	char files[10][50];
	struct arguments args;
	args.flag = 0;
	int cum_flag = 0;
	
	while ( ( c = getopt(argc, argv, "npd") ) != -1 ){
		switch(c){
			case 'n':
				cum_flag = cum_flag | NO_DEDUP;
				break;
			case 'p':
				cum_flag = cum_flag | PARTIAL_DEDUP;
				break;
			case 'd':
				cum_flag = cum_flag | DEBUG;
				break;
			case '?':
				if (isprint(optopt))
					fprintf(stderr, "Unknown option '-%c'.\n",optopt);
				else
					fprintf(stderr, "Unknown option character '\\x%x'.\n", optopt);
				return 1;
			default:
				abort();
		}
	}
	
	args.flag = cum_flag;

	i = 0;
	for (index = optind; index < argc; index++)
		strcpy(files[i++], argv[index]);

	if ( i < 2){
		printf("Insufficient number of arguments passed, expect at-least two input files\n");
		goto exit1;
	}


	if (((cum_flag == PARTIAL_DEDUP) || cum_flag  == (PARTIAL_DEDUP|DEBUG)) && i < 3){
		printf("Insufficient number of arguments passed, expect output file for -p flag\n");
		goto exit1;
	}

	j = 0;
	
	strcpy(args.inputfile1, files[j++]);
	strcpy(args.inputfile2, files[j++]);

	if ((cum_flag == PARTIAL_DEDUP) || cum_flag == (PARTIAL_DEDUP|DEBUG)){
		strcpy(args.outputfile, files[j++]);
	}
	
	rc = syscall(__NR_xdedup, (void *) &args);


	if (rc < 0){
		printf("Error: %s\n", strerror(-rc));
		goto exit1;
	} else if(cum_flag == 0){
		printf("Number of bytes deduped: %d\n", rc);
		goto exit1;
	} else if((cum_flag == NO_DEDUP) || (cum_flag == (NO_DEDUP|DEBUG)) || (cum_flag == (NO_DEDUP|PARTIAL_DEDUP)) || (cum_flag == (NO_DEDUP|PARTIAL_DEDUP|DEBUG))){
		printf("Number of bytes in common: %d\n", rc);
		goto exit1;
	} else if((cum_flag == PARTIAL_DEDUP) || (cum_flag == (PARTIAL_DEDUP|DEBUG))){
		printf("Number of bytes deduped: %d\n", rc);
		goto exit1;
	}

	exit1:
	exit(rc);
}
