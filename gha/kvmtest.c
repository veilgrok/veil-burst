#include <stdio.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/kvm.h>
int main(void){int k=open("/dev/kvm",O_RDWR);if(k<0){perror("KVMTEST open");return 1;}
int v=ioctl(k,KVM_GET_API_VERSION,0);int vm=ioctl(k,KVM_CREATE_VM,0);int vc=vm>=0?ioctl(vm,KVM_CREATE_VCPU,0):-1;
printf("KVMTEST api=%d create_vm=%s create_vcpu=%s\n",v,vm>=0?"ok":"FAIL",vc>=0?"ok":"FAIL");return 0;}
