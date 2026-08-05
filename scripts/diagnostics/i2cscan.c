#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <devctl.h>
#include <hw/i2c.h>

int main(void){
    for(int bus=0; bus<2; bus++){
        char dev[16]; snprintf(dev,sizeof dev,"/dev/i2c%d",bus);
        int fd=open(dev,O_RDWR);
        if(fd<0){ printf("%s: open failed (%s)\n",dev,strerror(errno)); continue; }
        printf("scanning %s ...\n",dev);
        int hits=0;
        for(int a=0x08;a<=0x77;a++){
            uint8_t b[sizeof(i2c_recv_t)+1];
            i2c_recv_t*h=(i2c_recv_t*)b;
            h->slave.addr=a; h->slave.fmt=I2C_ADDRFMT_7BIT; h->len=1; h->stop=1;
            if(devctl(fd,DCMD_I2C_RECV,b,sizeof b,NULL)==EOK){ printf("  0x%02x ACK\n",a); hits++; }
        }
        if(!hits) printf("  (nothing responded on %s)\n",dev);
        close(fd);
    }
    return 0;
}
