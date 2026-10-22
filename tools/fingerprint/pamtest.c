/* pamtest: drive pam_sm_authenticate of a module directly (no /etc/pam.d changes). */
#include <security/pam_appl.h>
#include <security/openpam.h>
#include <dlfcn.h>
#include <stdio.h>
#include <unistd.h>
typedef int (*authfn)(pam_handle_t*,int,int,const char**);
int main(int argc,const char**argv){
  struct pam_conv conv={openpam_ttyconv,NULL}; pam_handle_t*h;
  if(pam_start("other",getlogin(),&conv,&h)!=PAM_SUCCESS){puts("pam_start failed");return 1;}
  void*m=dlopen(argv[1],RTLD_NOW); if(!m){puts(dlerror());return 1;}
  authfn f=(authfn)dlsym(m,"pam_sm_authenticate");
  int r=f(h,0,argc-2,argv+2);
  printf("result=%d (%s)\n",r,pam_strerror(h,r)); return 0;}
