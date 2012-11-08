/* TODO: WRITE_MEM can come concurrently with recv(). use pending message list. */


#include <stdint.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/select.h>
#include <sys/time.h>
#include "hw.h"
#include "pci.h"
#include "msi.h"
#include "msix.h"
#include "qemu-common.h"
#include "qemu_socket.h"


#define CONFIG_USE_UDP 0

#define CONFIG_PCIEFW_DEBUG 1
#if CONFIG_PCIEFW_DEBUG
#include <stdio.h>
#define PRINTF(__s, ...)  \
do { printf("PCIEFW: " __s, ## __VA_ARGS__); } while (0)
#define PERROR() printf("PCIEFW: [!] %u\n", __LINE__)
#else
#define PRINTF(__s, ...)
#define PERROR()
#endif


/* pciefw device state */

typedef struct pciefw_props
{
  char* laddr;
  char* lport;
  char* raddr;
  char* rport;
} pciefw_props_t;

struct pciefw_state;

typedef struct pciefw_mmio
{
  /* use this structure as opaque to encode the bar */
  unsigned int bar;
  struct pciefw_state* state;
} pciefw_mmio_t;

struct pciefw_msg;

typedef struct pciefw_state
{
  PCIDevice dev;
  int sock;
  pciefw_props_t props;
#define PCI_NBARS 6
  MemoryRegion bar_region[PCI_NBARS];
  pciefw_mmio_t mmio[PCI_NBARS];
  size_t bar_size[PCI_NBARS];
  struct pciefw_msg* msg;
} pciefw_state_t;


/* message passing routines */

typedef struct pciefw_header
{
  uint16_t size;
} __attribute__((packed)) pciefw_header_t;

typedef struct pciefw_msg
{
#define PCIEFW_MSG_MAX_SIZE (offsetof(pciefw_msg_t, data) + 0x1000)

  pciefw_header_t header;

#define PCIEFW_OP_READ_CONFIG 0
#define PCIEFW_OP_WRITE_CONFIG 1
#define PCIEFW_OP_READ_MEM 2
#define PCIEFW_OP_WRITE_MEM 3
#define PCIEFW_OP_READ_IO 4
#define PCIEFW_OP_WRITE_IO 5
#define PCIEFW_OP_INT 6
#define PCIEFW_OP_MSI 7
#define PCIEFW_OP_MSIX 8

  uint8_t op; /* in PCIEFW_OP_XXX */
  uint8_t bar; /* in [0:5] */
  uint8_t width; /* access in 1, 2, 4, 8 */
  uint64_t addr;
  uint16_t size; /* data size, in bytes */
  uint8_t data[1];

} __attribute__((packed)) pciefw_msg_t;

typedef struct pciefw_reply
{
  pciefw_header_t header;
  uint8_t status;
  uint8_t data[8];
} __attribute__((packed)) pciefw_reply_t;

static ssize_t pciefw_recv_buf(int fd, void* buf, size_t max_size)
{
#if (CONFIG_USE_UDP == 1)
  ssize_t n;
  errno = 0;
  n = recv(fd, buf, max_size, 0);
  /* ignore ICMP_UNREACHABLE payloads */
  if (errno == ECONNREFUSED) return 0;
  if (n <= 0) { PERROR(); return -1; }
  return n;
#else
  pciefw_header_t h;
  ssize_t n;
  size_t rem_size;

  /* header always comes first */
  if (max_size < sizeof(h)) { PERROR(); return -1; }
  n = recv(fd, (void*)&h, sizeof(h), MSG_WAITALL);
  if (n != sizeof(h)) { PERROR(); return -1; }

  if (h.size > max_size) { PERROR(); return -1; }

  rem_size = h.size - sizeof(h);
  n = recv(fd, (uint8_t*)buf + sizeof(h), rem_size, MSG_WAITALL);
  if (n != (ssize_t)rem_size) { PERROR(); return -1; }
  return (ssize_t)h.size;
#endif /* (CONFIG_USE_UDP == 1) */
}

static int pciefw_send_buf(int fd, void* buf, size_t size)
{
  /* return 0 on success, -1 otherwise */

  ssize_t n;

#if (CONFIG_USE_UDP == 1)
 redo_send:
  errno = 0;
  n = send(fd, buf, size, 0);
  if (errno == ECONNREFUSED)
  {
    /* ignore ICMP_UNREACHABLE payloads */
    uint8_t dummy_buf;
    recv(fd, &dummy_buf, sizeof(dummy_buf), 0);
    goto redo_send;
  }
#else
  n = send(fd, buf, size, 0);
#endif /* CONFIG_USE_UDP */

  if (n != size) { PERROR(); return -1; }
  return 0;
}


static int pciefw_recv_reply(pciefw_state_t* state, pciefw_reply_t* r)
{
  while (1)
  {
    ssize_t n;
    fd_set fds;

    FD_ZERO(&fds);
    FD_SET(state->sock, &fds);

    errno = 0;
    if (select(state->sock + 1, &fds, NULL, NULL, NULL) <= 0)
    {
      if (errno == EINTR) continue ;

      PERROR();
      return -1;
    }

    n = pciefw_recv_buf(state->sock, (void*)r, sizeof(*r));
    if (n != 0) return (n == sizeof(*r)) ? 0 : -1;
    /* redo, icmp_unreachable case */
  }

  /* not reached */
  return 0;
}

static inline int pciefw_recv_msg(pciefw_state_t* state, pciefw_msg_t* m)
{
  const ssize_t n = pciefw_recv_buf(state->sock, (void*)m, PCIEFW_MSG_MAX_SIZE);
  if (n > 0) return 0;
  else if (n == 0) return 1; /* icmp_unreachable case */
  /* else, error */
  return -1;
}

static inline int pciefw_send_msg(pciefw_state_t* state, pciefw_msg_t* m)
{
  const size_t size = offsetof(pciefw_msg_t, data) + m->size;
  m->header.size = size;
  return pciefw_send_buf(state->sock, (void*)m, size);
}

static int pciefw_send_write_mem
(
 pciefw_state_t* state,
 unsigned int bar,
 uintptr_t addr,
 unsigned width,
 uintptr_t data
)
{
  pciefw_msg_t* const msg = state->msg;

  msg->op = PCIEFW_OP_WRITE_MEM;
  msg->bar = (uint8_t)bar;
  msg->width = (uint8_t)width;
  msg->addr = (uint64_t)addr;
  msg->size = (uint16_t)width;
  *(uint64_t*)msg->data = data;

  if (pciefw_send_msg(state, msg)) { PERROR(); return -1; }

  return 0;
}

static int pciefw_send_write_config
(pciefw_state_t* state, uintptr_t addr, unsigned int width, uint64_t data)
{
  pciefw_msg_t* const msg = state->msg;

  msg->op = PCIEFW_OP_WRITE_CONFIG;
  msg->width = (uint8_t)width;
  msg->addr = (uint64_t)addr;
  msg->size = (uint16_t)width;
  *(uint64_t*)msg->data = data;

  if (pciefw_send_msg(state, msg)) { PERROR(); return -1; }

  return 0;
}

__attribute__((unused))
static int pciefw_reply_read_config
(pciefw_state_t* state, uintptr_t addr, unsigned int type, uint64_t data)
{
  PRINTF("%s\n", __FUNCTION__);
  return 0;
}

static int pciefw_send_read_common
(pciefw_state_t* state, uintptr_t addr, unsigned int width, void* data)
{
  pciefw_msg_t* const msg = state->msg;
  pciefw_reply_t reply;

  msg->addr = (uint64_t)addr;
  msg->width = (uint8_t)width;
  msg->size = 0;

  if (pciefw_send_msg(state, msg)) { PERROR(); return -1; }
  if (pciefw_recv_reply(state, &reply)) { PERROR(); return -1; }

  switch (width)
  {
  case 1: *(uint8_t*)data = *(uint8_t*)reply.data; break ;
  case 2: *(uint16_t*)data = *(uint16_t*)reply.data; break ;
  case 4: *(uint32_t*)data = *(uint32_t*)reply.data; break ;
  case 8: *(uint64_t*)data = *(uint64_t*)reply.data; break ;
  default: break ;
  }

  return 0;
}

static inline int pciefw_send_read_config
(pciefw_state_t* state, uintptr_t addr, unsigned int width, void* data)
{
  pciefw_msg_t* const msg = state->msg;
  msg->op = PCIEFW_OP_READ_CONFIG;
  return pciefw_send_read_common(state, addr, width, data);
}

static inline int pciefw_send_read_mem
(
 pciefw_state_t* state,
 unsigned int bar,
 uintptr_t addr,
 unsigned int width,
 void* data
)
{
  pciefw_msg_t* const msg = state->msg;
  msg->op = PCIEFW_OP_READ_MEM;
  msg->bar = bar;
  return pciefw_send_read_common(state, addr, width, data);
}

__attribute__((unused))
static int pciefw_reply_write_mem
(pciefw_state_t* state, uintptr_t addr, unsigned int type, uint64_t data)
{
  PRINTF("%s\n", __FUNCTION__);
  return 0;
}

__attribute__((unused))
static int pciefw_send_msi(pciefw_state_t* state)
{
  PRINTF("%s\n", __FUNCTION__);
  return 0;
}


/* qemu device io operations */

static uint64_t pciefw_mmio_read
(void* opaque, target_phys_addr_t addr, unsigned width)
{
  pciefw_mmio_t* const mmio = opaque;
  uint64_t data = 0;
  int err;

  PRINTF("%s (%u+" TARGET_FMT_plx ")\n", __FUNCTION__, mmio->bar, addr);

  err = pciefw_send_read_mem(mmio->state, mmio->bar, addr, width, &data);
  return err ? (uint64_t)-1 : data;
}

static void pciefw_mmio_write
(void* opaque, target_phys_addr_t addr, uint64_t data, unsigned width)
{
  pciefw_mmio_t* const mmio = opaque;
  PRINTF("%s (%u+" TARGET_FMT_plx ", %lx)\n", __FUNCTION__, mmio->bar, addr, data);
  pciefw_send_write_mem(mmio->state, mmio->bar, addr, width, data);
}

static uint32_t pciefw_read_config(PCIDevice* dev, uint32_t addr, int width)
{
  pciefw_state_t* const state = DO_UPCAST(pciefw_state_t, dev, dev);

  uint64_t data = 0;

  PRINTF("%s (" TARGET_FMT_plx ")\n", __FUNCTION__, (unsigned long)addr);

#if 0
  return pci_default_read_config(dev, addr, width);
#else
  if (pciefw_send_read_config(state, addr, width, &data)) data = (uint64_t)-1;
  return (uint32_t)data;
#endif
}

static void pciefw_write_config
(PCIDevice* dev, uint32_t addr, uint32_t data, int width)
{
  pciefw_state_t* const state = DO_UPCAST(pciefw_state_t, dev, dev);

  PRINTF("%s (%x, %x, %u)\n", __FUNCTION__, addr, data, width);

  /* TODO: some write need to be filtered (PCI_BASE_ADDRESS_N) ? */

  pciefw_send_write_config(state, addr, width, data);
#if 1 /* TODO: useful? */
  pci_default_write_config(dev, addr, data, width);
  msi_write_config(dev, addr, data, width);
#endif
}

static const MemoryRegionOps pciefw_mmio_ops =
{
  .read = pciefw_mmio_read,
  .write = pciefw_mmio_write,
  .endianness = DEVICE_LITTLE_ENDIAN,
  .valid = { .min_access_size = 4, .max_access_size = 4 },
  .impl = { .min_access_size = 4, .max_access_size = 4 },
};


/* network io handlers */

static void pciefw_on_read(void* opaque)
{
  pciefw_state_t* const state = opaque;
  pciefw_msg_t* const msg = state->msg;
  int err;

  PRINTF("%s\n", __FUNCTION__);

  /* FIXME: polling needed, read would block */
  {
    struct timeval tm = { 0, 0 };
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(state->sock, &fds);
    if (select(state->sock + 1, &fds, NULL, NULL, &tm) <= 0)
    {
      PRINTF("nothing to read\n");
      return ;
    }
  }

  err = pciefw_recv_msg(state, msg);
  if (err == -1) { PERROR(); return ; }
  else if (err == 1) { return ; } /* icmp_unreachable case */

  switch (msg->op)
  {
  case PCIEFW_OP_WRITE_MEM:
    {
      int err;
      err = pci_dma_write
	(&state->dev, (dma_addr_t)msg->addr, msg->data, (dma_addr_t)msg->size);
      if (err) PRINTF("[!] pci_dma_write error\n");
      break ;
    }

  case PCIEFW_OP_MSI:
    msi_notify(&state->dev, 0);
    break ;

  default:
    PRINTF("unimplemented opcode: 0x%x\n", msg->op);
    break ;
  }
}

__attribute__((unused))
static void pciefw_on_write(void* opaque)
{
  /* pciefw_state_t* const state = opaque; */
  PRINTF("%s\n", __FUNCTION__);
}


/* pci device initialization */

static inline void check_props(pciefw_state_t* s)
{
  if (s->props.laddr == NULL) s->props.laddr = (char*)"127.0.0.1";
  if (s->props.lport == NULL) s->props.lport = (char*)"42424";
  if (s->props.raddr == NULL) s->props.raddr = (char*)"127.0.0.1";
  if (s->props.rport == NULL) s->props.rport = (char*)"42425";
}

static int pciefw_pci_init(PCIDevice* dev)
{
  pciefw_state_t* const state = DO_UPCAST(pciefw_state_t, dev, dev);
  unsigned int i;
  uint8_t* pci_conf;
  QemuOptsList* optlist;
  QemuOpts* opts;

  PRINTF("%s\n", __FUNCTION__);

  check_props(state);

  /* preallocate message buffer large enough */

  state->msg = g_malloc(PCIEFW_MSG_MAX_SIZE);
  if (state->msg == NULL) { return -1; }

  /* open inet socket */

  optlist = g_malloc0(offsetof(QemuOptsList, desc) + sizeof(QemuOptDesc));
  if (optlist == NULL) { g_free(state->msg); return -1; }

  optlist->name = "inet_optlist";
  optlist->head.tqh_first = NULL;
  optlist->head.tqh_last = &optlist->head.tqh_first;

  opts = qemu_opts_create(optlist, NULL, 0, NULL);
  if (opts == NULL)
  {
    PRINTF("opts == NULL\n");
    g_free(state->msg);
    g_free(optlist);
    return -1;
  }

  qemu_opt_set(opts, "host", state->props.raddr);
  qemu_opt_set(opts, "port", state->props.rport);
  qemu_opt_set(opts, "localaddr", state->props.laddr);
  qemu_opt_set(opts, "localport", state->props.lport);

#if (CONFIG_USE_UDP == 1)
  state->sock = inet_dgram_opts(opts);
#else
  optlist->desc[0].name = "block";
  optlist->desc[0].type = QEMU_OPT_BOOL;
  optlist->desc[0].help = "";
  qemu_opt_set_bool(opts, "block", true);
  state->sock = inet_connect_opts(opts, NULL, NULL);
#endif /* CONFIG_USE_UDP */

  qemu_opts_del(opts);
  g_free(optlist);

  if (state->sock == -1)
  {
    PRINTF("state->sock == -1\n");
    g_free(state->msg);
    return -1;
  }

  pci_conf = state->dev.config;
  pci_conf[PCI_COMMAND] = PCI_COMMAND_IO | PCI_COMMAND_MEMORY;

  /* read remote bars and register corresponding memory regions */

  for (i = 0; i < PCI_NBARS; ++i)
  {
    const uintptr_t config_addr = PCI_BASE_ADDRESS_0 + i * 4;
    uint32_t bar_size;

    state->bar_size[i] = 0;

    if (pciefw_send_write_config(state, config_addr, 4, (uint64_t)-1))
      { PERROR(); continue ; }

    if (pciefw_send_read_config(state, config_addr, 4, &bar_size))
      { PERROR(); continue ; }

#ifndef PCI_ADDR_FLAG_MASK
# define PCI_ADDR_FLAG_MASK 0xf
#endif
    bar_size &= ~PCI_ADDR_FLAG_MASK;
    if ((bar_size = (~bar_size + 1)) == 0) continue ;

    state->bar_size[i] = (size_t)bar_size;

    PRINTF("bar[%u]: " TARGET_FMT_plx "\n", i, state->bar_size[i]);

    state->mmio[i].bar = i;
    state->mmio[i].state = state;

    memory_region_init_io
    (
     &state->bar_region[i],
     &pciefw_mmio_ops,
     &state->mmio[i],
     "pciefw-mmio",
     state->bar_size[i]
    );

    pci_register_bar
    (
     &state->dev,
     i,
     PCI_BASE_ADDRESS_SPACE_MEMORY | PCI_BASE_ADDRESS_MEM_TYPE_64,
     &state->bar_region[i]
    );
  }

  /* initialize msi */
  /* TODO: check msi_enabled on remote device */
  if (msi_init(&state->dev, 0x00, 1, false, false) < 0) { PERROR(); }

  /* register the fd handler for qemu */
  qemu_set_fd_handler(state->sock, pciefw_on_read, NULL, state);

  return 0;
}

static void pciefw_pci_exit(PCIDevice* dev)
{
  pciefw_state_t* const state = DO_UPCAST(pciefw_state_t, dev, dev);

  unsigned int i;

  PRINTF("%s\n", __FUNCTION__);

  msi_uninit(dev);

  for (i = 0; i < PCI_NBARS; ++i)
  {
    if (state->bar_size[i] == 0) continue ;
    memory_region_destroy(&state->bar_region[i]);
  }

  qemu_set_fd_handler(state->sock, NULL, NULL, NULL);
  close(state->sock);

  g_free(state->msg);
}

static Property pciefw_props[] =
{
  /* networking */
  DEFINE_PROP_STRING("laddr", pciefw_state_t, props.laddr),
  DEFINE_PROP_STRING("lport", pciefw_state_t, props.lport),
  DEFINE_PROP_STRING("raddr", pciefw_state_t, props.raddr),
  DEFINE_PROP_STRING("rport", pciefw_state_t, props.rport),

  DEFINE_PROP_END_OF_LIST(),
};

static void pciefw_class_init(ObjectClass* klass, void* data)
{
  DeviceClass* const dc = DEVICE_CLASS(klass);
  PCIDeviceClass* const pdc = PCI_DEVICE_CLASS(klass);

  PRINTF("%s\n", __FUNCTION__);

  pdc->init = pciefw_pci_init;
  pdc->exit = pciefw_pci_exit;
  pdc->vendor_id = 0x2a2a;
  pdc->device_id = 0x2a2a;
  pdc->class_id = PCI_CLASS_OTHERS;
  pdc->config_read = pciefw_read_config;
  pdc->config_write = pciefw_write_config;

  dc->props = pciefw_props;
}

static TypeInfo pciefw_type_info =
{
  .name = "pciefw",
  .parent = TYPE_PCI_DEVICE,
  .instance_size = sizeof(pciefw_state_t),
  .class_init = pciefw_class_init,
};

static void pciefw_register_type(void)
{
  PRINTF("%s\n", __FUNCTION__);
  type_register_static(&pciefw_type_info);
}

type_init(pciefw_register_type)
