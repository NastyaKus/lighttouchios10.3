// SPDX-License-Identifier: GPL-3.0-or-later
#include <stdio.h>
#include <stdlib.h>
#include "../../guest/launchd-cache.h"
int main(int argc,char **argv) {
 if(argc!=3)return 2;
 FILE *f=fopen(argv[1],"rb");if(!f)return 3;
 fseek(f,0,SEEK_END);long size=ftell(f);rewind(f);
 if(size<0||size>32*1024*1024){fclose(f);return 3;}
 unsigned char *input=malloc((size_t)size? (size_t)size:1),*output=NULL;size_t n=0;
 if(fread(input,1,(size_t)size,f)!=(size_t)size){free(input);fclose(f);return 3;}fclose(f);
 const char *why=NULL;
 if(!lc_patch(input,(size_t)size,&output,&n,&why)){fprintf(stderr,"%s\n",why);free(input);return 1;}
 f=fopen(argv[2],"wb");if(!f){free(input);free(output);return 3;}
 int ok=fwrite(output,1,n,f)==n;fclose(f);free(input);free(output);return ok?0:3;
}
