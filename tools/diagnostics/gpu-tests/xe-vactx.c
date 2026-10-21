// xe-vactx: repete a abertura que o AppleIntelTGLGraphicsVADriver faz (Gen75IntelGVARendererCreateDriver) para ver
// qual chamada falha: IOAccelDeviceCreateWithAPIProperty("GVA") -> IOAccelSharedCreate -> IOAccelContextCreate 0x100/1/2.
// Só abre conexões com o kernel; não envia trabalho à GPU.
#include <IOKit/IOKitLib.h>
#include <stdio.h>
extern void *IOAccelDeviceCreateWithAPIProperty(io_service_t, const char *);
extern void *IOAccelSharedCreate(void *);
extern void *IOAccelContextCreate(void *, uint32_t);
extern io_connect_t IOAccelDeviceGetConnect(void *);
extern io_connect_t IOAccelVideoContextGetConnect(void *);
int main(void) {
  io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceNameMatching("IntelAccelerator"));
  printf("IntelAccelerator service = 0x%x\n", s);
  void *dev = IOAccelDeviceCreateWithAPIProperty(s, "GVA"); printf("device(GVA) = %p\n", dev); if (!dev) return 1;
  io_connect_t dc = IOAccelDeviceGetConnect(dev); printf("device connect = 0x%x\n", dc);
  uint32_t out[0x7c / 4] = {0}; size_t n = sizeof out;
  kern_return_t kr = IOConnectCallStructMethod(dc, 0xb, NULL, 0, out, &n);
  printf("get_hw_info (sel 0xb): kr=0x%x n=%zu device-id=0x%08x\n", kr, n, out[2]);
  void *sh = IOAccelSharedCreate(dev); printf("shared = %p\n", sh);
  for (uint32_t t = 0x100; t <= 0x102; t++) {
    void *c1 = IOAccelContextCreate(sh, t);
    printf("context 0x%x (com shared): %p connect=0x%x\n", t, c1, c1 ? IOAccelVideoContextGetConnect(c1) : 0);
    if (!c1) { void *c2 = IOAccelContextCreate(dev, t); printf("context 0x%x (com device): %p connect=0x%x\n", t, c2, c2 ? IOAccelVideoContextGetConnect(c2) : 0); }
  }
  return 0;
}
