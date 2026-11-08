/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "file-host.h"
#include <stdio.h>
int main(int argc,char **argv) {
    if(argc!=2){fprintf(stderr,"usage: host-probe NEW-scratch-image\n");return 2;}
    EgpuHostResult result;
    int status=egpu_file_host(argv[1],180000,NULL,NULL,&result);
    if(status)fprintf(stderr,"%s\n",result.error);
    return status;
}
