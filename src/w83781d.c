
/*
    w83781d.c - A Linux module for reading sensor data.
    Copyright (c) 1998  Frodo Looijaard <frodol@dds.nl>
    and Philip Edelbrock <phil@netroedge.com>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

#include <linux/module.h>
#include <linux/malloc.h>
#include <linux/proc_fs.h>
#include <linux/ioport.h>
#include <linux/sysctl.h>
#include <asm/errno.h>
#include <asm/io.h>
#include <linux/types.h>
#include "smbus.h"
#include "version.h"
#include "isa.h"
#include "sensors.h"
#include "i2c.h"
#include "compat.h"

/* Many W83781D constants specified below */

/* Length of ISA address segment */
#define W83781D_EXTENT 8

/* Where are the ISA address/data registers relative to the base address */
#define W83781D_ADDR_REG_OFFSET 5
#define W83781D_DATA_REG_OFFSET 6

/* The W83781D registers */
#define W83781D_REG_IN_MAX(nr) (0x2b + (nr) * 2)
#define W83781D_REG_IN_MIN(nr) (0x2c + (nr) * 2)
#define W83781D_REG_IN(nr) (0x20 + (nr))

#define W83781D_REG_FAN_MIN(nr) (0x3a + (nr))
#define W83781D_REG_FAN(nr) (0x27 + (nr))

#define W83781D_REG_TEMP23 0x50
#define W83781D_REG_TEMP_OVER23 0x55
#define W83781D_REG_TEMP_HYST23 0x53
#define W83781D_REG_TEMP 0x27
#define W83781D_REG_TEMP_OVER 0x39
#define W83781D_REG_TEMP_HYST 0x3A
#define W83781D_REG_TEMP_CONFIG 0x52
#define W83781D_REG_TEMP_SEL 0x4E

#define W83781D_REG_CONFIG 0x40
#define W83781D_REG_ALARM1 0x41
#define W83781D_REG_ALARM2 0x42

#define W83781D_REG_VID_FANDIV 0x47

#define W83781D_REG_CHIPID 0x58
#define W83781D_REG_CHIPMAN 0x4F


/* Conversions. Rounding is only done on the TO_REG variants. */
static int w83781d_in_conv[7] = {10000, 10000, 10000, 16892, 38000, 
                              -34768, -15050 };
#define IN_TO_REG(val,nr) (((((val) * 100000 / w83781d_in_conv[nr]) + 8) / 16) \
                           & 0xff)
#define IN_FROM_REG(val,nr) (((val) *  16 * w83781d_in_conv[nr]) / 100000)

#define FAN_TO_REG(val) ((val)==0?255:((1350000+(val))/((val)*2)) & 0xff)
#define FAN_FROM_REG(val) ((val)==0?-1:(val)==255?0:1350000/((val)*2))

#define TEMP_TO_REG(val) (((val)<0?(((val)-5)/10)&0xff:((val)+5)/10) & 0xff)
#define TEMP_FROM_REG(val) (((val)>0x80?(val)-0x100:(val))*10)

#define VID_FROM_REG(val) ((val)==0x1f?0:(val)>=0x10?510-(val)*10:\
                           (val)>=0x06?0:205-(val)*5)
#define ALARMS_FROM_REG(val) (val)

#define DIV_FROM_REG(val) (1 << (val))
#define DIV_TO_REG(val) ((val)==8?3:(val)==4?2:(val)==1?0:1)

/* Initial limits */
#define W83781D_INIT_IN_0 (vid==350?280:vid)
#define W83781D_INIT_IN_1 (vid==350?280:vid)
#define W83781D_INIT_IN_2 330
#define W83781D_INIT_IN_3 500
#define W83781D_INIT_IN_4 1200
#define W83781D_INIT_IN_5 -1200
#define W83781D_INIT_IN_6 -500

#define W83781D_INIT_IN_PERCENTAGE 10

#define W83781D_INIT_IN_MIN_0 \
        (W83781D_INIT_IN_0 - W83781D_INIT_IN_0 * W83781D_INIT_IN_PERCENTAGE / 100) 
#define W83781D_INIT_IN_MAX_0 \
        (W83781D_INIT_IN_0 + W83781D_INIT_IN_0 * W83781D_INIT_IN_PERCENTAGE / 100) 
#define W83781D_INIT_IN_MIN_1 \
        (W83781D_INIT_IN_1 - W83781D_INIT_IN_1 * W83781D_INIT_IN_PERCENTAGE / 100) 
#define W83781D_INIT_IN_MAX_1 \
        (W83781D_INIT_IN_1 + W83781D_INIT_IN_1 * W83781D_INIT_IN_PERCENTAGE / 100) 
#define W83781D_INIT_IN_MIN_2 \
        (W83781D_INIT_IN_2 - W83781D_INIT_IN_2 * W83781D_INIT_IN_PERCENTAGE / 100) 
#define W83781D_INIT_IN_MAX_2 \
        (W83781D_INIT_IN_2 + W83781D_INIT_IN_2 * W83781D_INIT_IN_PERCENTAGE / 100) 
#define W83781D_INIT_IN_MIN_3 \
        (W83781D_INIT_IN_3 - W83781D_INIT_IN_3 * W83781D_INIT_IN_PERCENTAGE / 100) 
#define W83781D_INIT_IN_MAX_3 \
        (W83781D_INIT_IN_3 + W83781D_INIT_IN_3 * W83781D_INIT_IN_PERCENTAGE / 100) 
#define W83781D_INIT_IN_MIN_4 \
        (W83781D_INIT_IN_4 - W83781D_INIT_IN_4 * W83781D_INIT_IN_PERCENTAGE / 100) 
#define W83781D_INIT_IN_MAX_4 \
        (W83781D_INIT_IN_4 + W83781D_INIT_IN_4 * W83781D_INIT_IN_PERCENTAGE / 100) 
#define W83781D_INIT_IN_MIN_5 \
        (W83781D_INIT_IN_5 - W83781D_INIT_IN_5 * W83781D_INIT_IN_PERCENTAGE / 100) 
#define W83781D_INIT_IN_MAX_5 \
        (W83781D_INIT_IN_5 + W83781D_INIT_IN_5 * W83781D_INIT_IN_PERCENTAGE / 100) 
#define W83781D_INIT_IN_MIN_6 \
        (W83781D_INIT_IN_6 - W83781D_INIT_IN_6 * W83781D_INIT_IN_PERCENTAGE / 100) 
#define W83781D_INIT_IN_MAX_6 \
        (W83781D_INIT_IN_6 + W83781D_INIT_IN_6 * W83781D_INIT_IN_PERCENTAGE / 100) 

#define W83781D_INIT_FAN_MIN_1 3000
#define W83781D_INIT_FAN_MIN_2 3000
#define W83781D_INIT_FAN_MIN_3 3000

#define W83781D_INIT_TEMP_OVER 600
#define W83781D_INIT_TEMP_HYST 500

#ifdef MODULE
extern int init_module(void);
extern int cleanup_module(void);
#endif /* MODULE */

/* There are some complications in a module like this. First off, W83781D chips
   may be both present on the SMBus and the ISA bus, and we have to handle
   those cases separately at some places. Second, there might be several
   W83781D chips available (well, actually, that is probably never done; but
   it is a clean illustration of how to handle a case like that). Finally,
   a specific chip may be attached to *both* ISA and SMBus, and we would
   not like to detect it double. Fortunately, in the case of the W83781D at
   least, a register tells us what SMBus address we are on, so that helps
   a bit - except if there could be more than one SMBus. Groan. No solution
   for this yet. */

/* This module may seem overly long and complicated. In fact, it is not so
   bad. Quite a lot of bookkeeping is done. A real driver can often cut
   some corners. */

/* For each registered W83781D, we need to keep some data in memory. That
   data is pointed to by w83781d_list[NR]->data. The structure itself is
   dynamically allocated, at the same time when a new w83781d client is
   allocated. */
struct w83781d_data {
         struct semaphore lock;
         int sysctl_id;

         struct semaphore update_lock;
         char valid;                 /* !=0 if following fields are valid */
         unsigned long last_updated; /* In jiffies */

         u8 in[7];                   /* Register value */
         u8 in_max[7];               /* Register value */
         u8 in_min[7];               /* Register value */
         u8 fan[3];                  /* Register value */
         u8 fan_min[3];              /* Register value */
         u8 temp[3];                    /* Register value */
         u8 temp_over[3];               /* Register value */
         u8 temp_hyst[3];               /* Register value */
         u8 fan_div[2];              /* Register encoding, shifted right */
         u8 vid;                     /* Register encoding, combined */
         u16 alarms;                 /* Register encoding, combined */
};


static int w83781d_init(void);
static int w83781d_cleanup(void);

static int w83781d_attach_adapter(struct i2c_adapter *adapter);
static int w83781d_detect_isa(struct isa_adapter *adapter);
static int w83781d_detect_smbus(struct i2c_adapter *adapter);
static int w83781d_detach_client(struct i2c_client *client);
static int w83781d_detach_isa(struct isa_client *client);
static int w83781d_detach_smbus(struct i2c_client *client);
static int w83781d_new_client(struct i2c_adapter *adapter,
                           struct i2c_client *new_client);
static void w83781d_remove_client(struct i2c_client *client);
static int w83781d_command(struct i2c_client *client, unsigned int cmd, 
                        void *arg);
static void w83781d_inc_use (struct i2c_client *client);
static void w83781d_dec_use (struct i2c_client *client);

static int w83781d_read_value(struct i2c_client *client, u8 register);
static int w83781d_write_value(struct i2c_client *client, u8 register, u8 value);
static void w83781d_update_client(struct i2c_client *client);
static void w83781d_init_client(struct i2c_client *client);


static void w83781d_in(struct i2c_client *client, int operation, int ctl_name,
                    int *nrels_mag, long *results);
static void w83781d_fan(struct i2c_client *client, int operation, int ctl_name,
                     int *nrels_mag, long *results);
static void w83781d_temp(struct i2c_client *client, int operation, int ctl_name,
                      int *nrels_mag, long *results,int tempnum);
static void w83781d_temp1(struct i2c_client *client, int operation, int ctl_name,
                      int *nrels_mag, long *results);
static void w83781d_temp2(struct i2c_client *client, int operation, int ctl_name,
                      int *nrels_mag, long *results);
static void w83781d_temp3(struct i2c_client *client, int operation, int ctl_name,
                      int *nrels_mag, long *results);
static void w83781d_vid(struct i2c_client *client, int operation, int ctl_name,
                     int *nrels_mag, long *results);
static void w83781d_alarms(struct i2c_client *client, int operation, int ctl_name,
                        int *nrels_mag, long *results);
static void w83781d_fan_div(struct i2c_client *client, int operation, int ctl_name,
                         int *nrels_mag, long *results);

/* I choose here for semi-static W83781D allocation. Complete dynamic
   allocation could also be used; the code needed for this would probably
   take more memory than the datastructure takes now. */
#define MAX_W83781D_NR 4
static struct i2c_client *w83781d_list[MAX_W83781D_NR];

/* The driver. I choose to use type i2c_driver, as at is identical to both
   smbus_driver and isa_driver, and clients could be of either kind */
static struct i2c_driver w83781d_driver = {
  /* name */		"W83781D sensor driver",
  /* id */		I2C_DRIVERID_W83781D,
  /* flags */		DF_NOTIFY,
  /* attach_adapter */  &w83781d_attach_adapter,
  /* detach_client */	&w83781d_detach_client,
  /* command */		&w83781d_command,
  /* inc_use */		&w83781d_inc_use,
  /* dec_use */		&w83781d_dec_use
};

/* Used by w83781d_init/cleanup */
static int w83781d_initialized = 0;

/* The /proc/sys entries */
/* These files are created for each detected W83781D. This is just a template;
   though at first sight, you might think we could use a statically
   allocated list, we need some way to get back to the parent - which
   is done through one of the 'extra' fields which are initialized 
   when a new copy is allocated. */
static ctl_table w83781d_dir_table_template[] = {
  { W83781D_SYSCTL_IN0, "in0", NULL, 0, 0644, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_in },
  { W83781D_SYSCTL_IN1, "in1", NULL, 0, 0644, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_in },
  { W83781D_SYSCTL_IN2, "in2", NULL, 0, 0644, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_in },
  { W83781D_SYSCTL_IN3, "in3", NULL, 0, 0644, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_in },
  { W83781D_SYSCTL_IN4, "in4", NULL, 0, 0644, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_in },
  { W83781D_SYSCTL_IN5, "in5", NULL, 0, 0644, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_in },
  { W83781D_SYSCTL_IN6, "in6", NULL, 0, 0644, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_in },
  { W83781D_SYSCTL_FAN1, "fan1", NULL, 0, 0644, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_fan },
  { W83781D_SYSCTL_FAN2, "fan2", NULL, 0, 0644, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_fan },
  { W83781D_SYSCTL_FAN3, "fan3", NULL, 0, 0644, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_fan },
  { W83781D_SYSCTL_TEMP, "temp", NULL, 0, 0644, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_temp1 },
  { W83781D_SYSCTL_TEMP2, "temp2", NULL, 0, 0644, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_temp2 },
  { W83781D_SYSCTL_TEMP3, "temp3", NULL, 0, 0644, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_temp3 },
  { W83781D_SYSCTL_VID, "vid", NULL, 0, 0644, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_vid },
  { W83781D_SYSCTL_FAN_DIV, "fan_div", NULL, 0, 0644, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_fan_div },
  { W83781D_SYSCTL_ALARMS, "alarms", NULL, 0, 0644, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_alarms },
  { 0 }
};


/* This function is called when:
     * w83781d_driver is inserted (when this module is loaded), for each
       available adapter
     * when a new adapter is inserted (and w83781d_driver is still present) */
int w83781d_attach_adapter(struct i2c_adapter *adapter)
{
  if (i2c_is_isa_adapter(adapter))
    return w83781d_detect_isa((struct isa_adapter *) adapter);
  else
    return w83781d_detect_smbus(adapter);
}

/* This function is called whenever a client should be removed:
    * w83781d_driver is removed (when this module is unloaded)
    * when an adapter is removed which has a w83781d client (and w83781d_driver
      is still present). */
int w83781d_detach_client(struct i2c_client *client)
{
  if (i2c_is_isa_client(client))
    return w83781d_detach_isa((struct isa_client *) client);
  else
    return w83781d_detach_smbus(client);
}

/* Detect whether there is a W83781D on the ISA bus, register and initialize 
   it. */
int w83781d_detect_isa(struct isa_adapter *adapter)
{
  int address,err;
  struct isa_client *new_client;
  const char *type_name;
  const char *client_name;

  /* OK, this is no detection. I know. It will do for now, though.  */

  err = 0;
  for (address = 0x290; (! err) && (address <= 0x290); address += 0x08) {
    if (check_region(address, W83781D_EXTENT))
      continue;

    /* Awful, but true: unused port addresses should return 0xff */
    if ((inb_p(address + 1) != 0xff) || (inb_p(address + 2) != 0xff) ||
       (inb_p(address + 3) != 0xff) || (inb_p(address + 7) != 0xff))
      continue;
    
    if (inb_p(address + W83781D_ADDR_REG_OFFSET) == 0xff) {
      outb_p(0x00,address + W83781D_ADDR_REG_OFFSET);
      if (inb_p(address + W83781D_ADDR_REG_OFFSET) == 0xff)
        continue;
    }
    
    /* Real detection code goes here */

    outb_p(W83781D_REG_CHIPID,address + W83781D_ADDR_REG_OFFSET);
    err = inb_p(address + W83781D_DATA_REG_OFFSET) & 0xfe;

    if (err != 0x20) {
      printk("w83781d.o: Winbond W83781D detected (ISA addr=0x%X)\n",address);
      type_name = "w83781d";
      client_name = "Winbond W83781D chip";
    } else {
 #ifdef DEBUG
     printk("83781d.o: Winbond W83781D not detected (ISA)\n");
 #endif
     continue;
    }

    request_region(address, W83781D_EXTENT, type_name);

    /* Allocate space for a new client structure */
    if (! (new_client = kmalloc(sizeof(struct isa_client) + 
                                sizeof(struct w83781d_data),
                               GFP_KERNEL)))
    {
      err=-ENOMEM;
      goto ERROR1;
    } 

    /* Fill the new client structure with data */
    new_client->data = (struct w83781d_data *) (new_client + 1);
    new_client->addr = 0;
    strcpy(new_client->name,client_name);
    new_client->isa_addr = address;
    if ((err = w83781d_new_client((struct i2c_adapter *) adapter,
                               (struct i2c_client *) new_client)))
      goto ERROR2;

    /* Tell i2c-core a new client has arrived */
    if ((err = isa_attach_client(new_client)))
      goto ERROR3;
    
    /* Register a new directory entry with module sensors */
    if ((err = sensors_register_entry((struct i2c_client *) new_client,
                                      type_name,
                                      w83781d_dir_table_template)) < 0)
      goto ERROR4;
    ((struct w83781d_data *) (new_client->data)) -> sysctl_id = err;
    err = 0;

    /* Initialize the W83781D chip */
    w83781d_init_client((struct i2c_client *) new_client);
    continue;

/* OK, this is not exactly good programming practice, usually. But it is
   very code-efficient in this case. */

ERROR4:
    isa_detach_client(new_client);
ERROR3:
    w83781d_remove_client((struct i2c_client *) new_client);
ERROR2:
    kfree(new_client);
ERROR1:
    release_region(address, W83781D_EXTENT);
  }
  return err;

}

/* Deregister and remove a W83781D client */
int w83781d_detach_isa(struct isa_client *client)
{
  int err,i;
  for (i = 0; i < MAX_W83781D_NR; i++)
    if ((client == (struct isa_client *) (w83781d_list[i])))
      break;
  if (i == MAX_W83781D_NR) {
    printk("w83781d.o: Client to detach not found.\n");
    return -ENOENT;
  }

  sensors_deregister_entry(((struct w83781d_data *)(client->data))->sysctl_id);

  if ((err = isa_detach_client(client))) {
    printk("w83781d.o: Client deregistration failed, client not detached.\n");
    return err;
  }
  w83781d_remove_client((struct i2c_client *) client);
  release_region(client->isa_addr,W83781D_EXTENT);
  kfree(client);
  return 0;
}

int w83781d_detect_smbus(struct i2c_adapter *adapter)
{
  int address,err;
  struct i2c_client *new_client;
  const char *type_name,*client_name;

  /* OK, this is no detection. I know. It will do for now, though.  */
  err = 0;
  for (address = 0x20; (! err) && (address <= 0x2f); address ++) {

    /* Later on, we will keep a list of registered addresses for each
       adapter, and check whether they are used here */

    if (smbus_read_byte_data(adapter,address,W83781D_REG_CONFIG) < 0) 
      continue;

    err = smbus_read_byte_data(adapter,address,W83781D_REG_CHIPID);
    
    if (err == 0x20) {
      printk("w83781d.o: Winbond W83781D detected (SMBus addr 0x%X)\n",address);
      type_name = "w83781d";
      client_name = "Winbond W83781D chip";
    } else {
 #ifdef DEBUG
     printk("83781d.o: Winbond W83781D not detected (SMBus/I2C)\n");
 #endif
     continue;
    }

    /* Allocate space for a new client structure. To counter memory
       ragmentation somewhat, we only do one kmalloc. */
    if (! (new_client = kmalloc(sizeof(struct i2c_client) + 
                                sizeof(struct w83781d_data),
                               GFP_KERNEL))) {
      err = -ENOMEM;
      continue;
    }

    /* Fill the new client structure with data */
    new_client->data = (struct w83781d_data *) (new_client + 1);
    new_client->addr = address;
    strcpy(new_client->name,client_name);
    if ((err = w83781d_new_client(adapter,new_client)))
      goto ERROR2;

    /* Tell i2c-core a new client has arrived */
    if ((err = i2c_attach_client(new_client))) 
      goto ERROR3;

    /* Register a new directory entry with module sensors */
    if ((err = sensors_register_entry(new_client,type_name,
                                      w83781d_dir_table_template)) < 0)
      goto ERROR4;
    ((struct w83781d_data *) (new_client->data))->sysctl_id = err;
    err = 0;

    /* Initialize the W83781D chip */
    w83781d_init_client(new_client);
    continue;

/* OK, this is not exactly good programming practice, usually. But it is
   very code-efficient in this case. */
ERROR4:
    i2c_detach_client(new_client);
ERROR3:
    w83781d_remove_client((struct i2c_client *) new_client);
ERROR2:
    kfree(new_client);
  }
  return err;
}

int w83781d_detach_smbus(struct i2c_client *client)
{
  int err,i;
  for (i = 0; i < MAX_W83781D_NR; i++)
    if (client == w83781d_list[i])
      break;
  if ((i == MAX_W83781D_NR)) {
    printk("w83781d.o: Client to detach not found.\n");
    return -ENOENT;
  }

  sensors_deregister_entry(((struct w83781d_data *)(client->data))->sysctl_id);

  if ((err = i2c_detach_client(client))) {
    printk("w83781d.o: Client deregistration failed, client not detached.\n");
    return err;
  }
  w83781d_remove_client(client);
  kfree(client);
  return 0;
}


/* Find a free slot, and initialize most of the fields */
int w83781d_new_client(struct i2c_adapter *adapter,
                    struct i2c_client *new_client)
{
  int i;
  struct w83781d_data *data;

  /* First, seek out an empty slot */
  for(i = 0; i < MAX_W83781D_NR; i++)
    if (! w83781d_list[i])
      break;
  if (i == MAX_W83781D_NR) {
    printk("w83781d.o: No empty slots left, recompile and heighten "
           "MAX_W83781D_NR!\n");
    return -ENOMEM;
  }
  
  w83781d_list[i] = new_client;
  new_client->id = i;
  new_client->adapter = adapter;
  new_client->driver = &w83781d_driver;
  data = new_client->data;
  data->valid = 0;
  data->lock = MUTEX;
  data->update_lock = MUTEX;
  return 0;
}

/* Inverse of w83781d_new_client */
void w83781d_remove_client(struct i2c_client *client)
{
  int i;
  for (i = 0; i < MAX_W83781D_NR; i++)
    if (client == w83781d_list[i]) 
      w83781d_list[i] = NULL;
}

/* No commands defined yet */
int w83781d_command(struct i2c_client *client, unsigned int cmd, void *arg)
{
  return 0;
}

/* Nothing here yet */
void w83781d_inc_use (struct i2c_client *client)
{
#ifdef MODULE
  MOD_INC_USE_COUNT;
#endif
}

/* Nothing here yet */
void w83781d_dec_use (struct i2c_client *client)
{
#ifdef MODULE
  MOD_DEC_USE_COUNT;
#endif
}
 

/* The SMBus locks itself, but ISA access must be locked explicitely! 
   We ignore the W83781D BUSY flag at this moment - it could lead to deadlocks,
   would slow down the W83781D access and should not be necessary. 
   There are some ugly typecasts here, but the good new is - they should
   nowhere else be necessary! */
int w83781d_read_value(struct i2c_client *client, u8 reg)
{
  int res;
  if (i2c_is_isa_client(client)) {
    down((struct semaphore *) (client->data));
    outb_p(reg,(((struct isa_client *) client)->isa_addr) + 
               W83781D_ADDR_REG_OFFSET);
    res = inb_p((((struct isa_client *) client)->isa_addr) + 
                W83781D_DATA_REG_OFFSET);
    up((struct semaphore *) (client->data));
    return res;
  } else
    return smbus_read_byte_data(client->adapter,client->addr, reg);
}

/* The SMBus locks itself, but ISA access muse be locked explicitely! 
   We ignore the W83781D BUSY flag at this moment - it could lead to deadlocks,
   would slow down the W83781D access and should not be necessary. 
   There are some ugly typecasts here, but the good new is - they should
   nowhere else be necessary! */
int w83781d_write_value(struct i2c_client *client, u8 reg, u8 value)
{
  if (i2c_is_isa_client(client)) {
    down((struct semaphore *) (client->data));
    outb_p(reg,((struct isa_client *) client)->isa_addr + W83781D_ADDR_REG_OFFSET);
    outb_p(value,((struct isa_client *) client)->isa_addr + W83781D_DATA_REG_OFFSET);
    up((struct semaphore *) (client->data));
    return 0;
  } else
    return smbus_write_byte_data(client->adapter, client->addr, reg,value);
}

/* Called when we have found a new W83781D. It should set limits, etc. */
void w83781d_init_client(struct i2c_client *client)
{
  int vid;

  /* Reset all except Watchdog values and last conversion values
     This sets fan-divs to 2, among others */
  w83781d_write_value(client,W83781D_REG_CONFIG,0x80);

  vid = w83781d_read_value(client,W83781D_REG_VID_FANDIV) & 0x0f;
  vid |= (w83781d_read_value(client,W83781D_REG_CHIPID) & 0x01) >> 4;
  vid = VID_FROM_REG(vid);

  w83781d_write_value(client,W83781D_REG_IN_MIN(0),IN_TO_REG(W83781D_INIT_IN_MIN_0,0));
  w83781d_write_value(client,W83781D_REG_IN_MAX(0),IN_TO_REG(W83781D_INIT_IN_MAX_0,0));
  w83781d_write_value(client,W83781D_REG_IN_MIN(1),IN_TO_REG(W83781D_INIT_IN_MIN_1,1));
  w83781d_write_value(client,W83781D_REG_IN_MAX(1),IN_TO_REG(W83781D_INIT_IN_MAX_1,1));
  w83781d_write_value(client,W83781D_REG_IN_MIN(2),IN_TO_REG(W83781D_INIT_IN_MIN_2,2));
  w83781d_write_value(client,W83781D_REG_IN_MAX(2),IN_TO_REG(W83781D_INIT_IN_MAX_2,2));
  w83781d_write_value(client,W83781D_REG_IN_MIN(3),IN_TO_REG(W83781D_INIT_IN_MIN_3,3));
  w83781d_write_value(client,W83781D_REG_IN_MAX(3),IN_TO_REG(W83781D_INIT_IN_MAX_3,3));
  w83781d_write_value(client,W83781D_REG_IN_MIN(4),IN_TO_REG(W83781D_INIT_IN_MIN_4,4));
  w83781d_write_value(client,W83781D_REG_IN_MAX(4),IN_TO_REG(W83781D_INIT_IN_MAX_4,4));
  w83781d_write_value(client,W83781D_REG_IN_MIN(5),IN_TO_REG(W83781D_INIT_IN_MIN_5,5));
  w83781d_write_value(client,W83781D_REG_IN_MAX(5),IN_TO_REG(W83781D_INIT_IN_MAX_5,5));
  w83781d_write_value(client,W83781D_REG_IN_MIN(6),IN_TO_REG(W83781D_INIT_IN_MIN_6,6));
  w83781d_write_value(client,W83781D_REG_IN_MAX(6),IN_TO_REG(W83781D_INIT_IN_MAX_6,6));
  w83781d_write_value(client,W83781D_REG_FAN_MIN(1),FAN_TO_REG(W83781D_INIT_FAN_MIN_1));
  w83781d_write_value(client,W83781D_REG_FAN_MIN(2),FAN_TO_REG(W83781D_INIT_FAN_MIN_2));
  w83781d_write_value(client,W83781D_REG_FAN_MIN(3),FAN_TO_REG(W83781D_INIT_FAN_MIN_3));
  /* Init Temp Sensor1 */
  w83781d_write_value(client,W83781D_REG_TEMP_SEL,0x01);/* Switch Banks! */
  w83781d_write_value(client,W83781D_REG_TEMP_OVER,TEMP_TO_REG(W83781D_INIT_TEMP_OVER));
  w83781d_write_value(client,W83781D_REG_TEMP_HYST,TEMP_TO_REG(W83781D_INIT_TEMP_HYST));
  /* Init Temp Sensor2 */
  w83781d_write_value(client,W83781D_REG_TEMP_SEL,0x02);/* Switch Banks! */
  w83781d_write_value(client,W83781D_REG_TEMP_OVER23,TEMP_TO_REG(W83781D_INIT_TEMP_OVER));
  w83781d_write_value(client,W83781D_REG_TEMP_OVER23 + 1,0);
  w83781d_write_value(client,W83781D_REG_TEMP_HYST23,TEMP_TO_REG(W83781D_INIT_TEMP_HYST));
  w83781d_write_value(client,W83781D_REG_TEMP_HYST23 + 1,0);
  w83781d_write_value(client,W83781D_REG_TEMP_CONFIG,0x00);
  /* Init Temp Sensor3 */
  w83781d_write_value(client,W83781D_REG_TEMP_SEL,0x04);/* Switch Banks! */
  w83781d_write_value(client,W83781D_REG_TEMP_OVER23,TEMP_TO_REG(W83781D_INIT_TEMP_OVER));
  w83781d_write_value(client,W83781D_REG_TEMP_OVER23 + 1,0);
  w83781d_write_value(client,W83781D_REG_TEMP_HYST23,TEMP_TO_REG(W83781D_INIT_TEMP_HYST));
  w83781d_write_value(client,W83781D_REG_TEMP_HYST23 + 1,0);
  w83781d_write_value(client,W83781D_REG_TEMP_CONFIG,0x00);
  w83781d_write_value(client,W83781D_REG_TEMP_SEL,0x00);/* Switch Banks! */

  /* Start monitoring */
  w83781d_write_value(client,W83781D_REG_CONFIG,
                   (w83781d_read_value(client,W83781D_REG_CONFIG) & 0xf7) | 0x01);
  
}

void w83781d_update_client(struct i2c_client *client)
{
  struct w83781d_data *data = client->data;
  int i;

  down(&data->update_lock);

  if ((jiffies - data->last_updated > HZ+HZ/2 ) ||
      (jiffies < data->last_updated) || ! data->valid) {

#ifdef DEBUG
    printk("Starting w83781d update\n");
#endif
    for (i = 0; i <= 6; i++) {
      data->in[i]     = w83781d_read_value(client,W83781D_REG_IN(i));
      data->in_min[i] = w83781d_read_value(client,W83781D_REG_IN_MIN(i));
      data->in_max[i] = w83781d_read_value(client,W83781D_REG_IN_MAX(i));
    }
    for (i = 1; i <= 3; i++) {
      data->fan[i-1] = w83781d_read_value(client,W83781D_REG_FAN(i));
      data->fan_min[i-1] = w83781d_read_value(client,W83781D_REG_FAN_MIN(i));
    }
    w83781d_write_value(client,W83781D_REG_TEMP_SEL,1);/* Switch Banks!! */
    data->temp[0] = w83781d_read_value(client,W83781D_REG_TEMP);
    data->temp_over[0] = w83781d_read_value(client,W83781D_REG_TEMP_OVER);
    data->temp_hyst[0] = w83781d_read_value(client,W83781D_REG_TEMP_HYST);
    for (i = 1; i <= 2; i++) {
      w83781d_write_value(client,W83781D_REG_TEMP_SEL,1 << i);/* Switch Banks!! */
      data->temp[i] = w83781d_read_value(client,W83781D_REG_TEMP23);
      data->temp_over[i] = w83781d_read_value(client,W83781D_REG_TEMP_OVER23);
      data->temp_hyst[i] = w83781d_read_value(client,W83781D_REG_TEMP_HYST23);
    }
    w83781d_write_value(client,W83781D_REG_TEMP_SEL,0);/* Switch Banks!! */
    i = w83781d_read_value(client,W83781D_REG_VID_FANDIV);
    data->vid = i & 0x0f;
    data->vid |= (w83781d_read_value(client,W83781D_REG_CHIPID) & 0x01) >> 4;
    data->fan_div[0] = (i >> 4) & 0x03;
    data->fan_div[1] = i >> 6;
    data->alarms = w83781d_read_value(client,W83781D_REG_ALARM1) +
                   (w83781d_read_value(client,W83781D_REG_ALARM2) >> 8);
    data->last_updated = jiffies;
    data->valid = 1;
  }

  up(&data->update_lock);
}


/* The next few functions are the call-back functions of the /proc/sys and
   sysctl files. Which function is used is defined in the ctl_table in
   the extra1 field.
   Each function must return the magnitude (power of 10 to divide the date
   with) if it is called with operation==SENSORS_PROC_REAL_INFO. It must
   put a maximum of *nrels elements in results reflecting the data of this
   file, and set *nrels to the number it actually put in it, if operation==
   SENSORS_PROC_REAL_READ. Finally, it must get upto *nrels elements from
   results and write them to the chip, if operations==SENSORS_PROC_REAL_WRITE.
   Note that on SENSORS_PROC_REAL_READ, I do not check whether results is
   large enough (by checking the incoming value of *nrels). This is not very
   good practice, but as long as you put less than about 5 values in results,
   you can assume it is large enough. */
void w83781d_in(struct i2c_client *client, int operation, int ctl_name, 
             int *nrels_mag, long *results)
{
  struct w83781d_data *data = client->data;
  int nr = ctl_name - W83781D_SYSCTL_IN0;

  if (operation == SENSORS_PROC_REAL_INFO)
    *nrels_mag = 2;
  else if (operation == SENSORS_PROC_REAL_READ) {
    w83781d_update_client(client);
    results[0] = IN_FROM_REG(data->in_min[nr],nr);
    results[1] = IN_FROM_REG(data->in_max[nr],nr);
    results[2] = IN_FROM_REG(data->in[nr],nr);
    *nrels_mag = 3;
  } else if (operation == SENSORS_PROC_REAL_WRITE) {
      if (*nrels_mag >= 1) {
        data->in_min[nr] = IN_TO_REG(results[0],nr);
        w83781d_write_value(client,W83781D_REG_IN_MIN(nr),data->in_min[nr]);
      }
      if (*nrels_mag >= 2) {
        data->in_max[nr] = IN_TO_REG(results[1],nr);
        w83781d_write_value(client,W83781D_REG_IN_MAX(nr),data->in_max[nr]);
      }
  }
}

void w83781d_fan(struct i2c_client *client, int operation, int ctl_name,
              int *nrels_mag, long *results)
{
  struct w83781d_data *data = client->data;
  int nr = ctl_name - W83781D_SYSCTL_FAN1 + 1;

  if (operation == SENSORS_PROC_REAL_INFO)
    *nrels_mag = 0;
  else if (operation == SENSORS_PROC_REAL_READ) {
    w83781d_update_client(client);
    results[0] = FAN_FROM_REG(data->fan_min[nr-1]);
    results[1] = FAN_FROM_REG(data->fan[nr-1]);
    *nrels_mag = 2;
  } else if (operation == SENSORS_PROC_REAL_WRITE) {
    if (*nrels_mag >= 1) {
      data->fan_min[nr-1] = FAN_TO_REG(results[0]);
      w83781d_write_value(client,W83781D_REG_FAN_MIN(nr),data->fan_min[nr-1]);
    }
  }
}


void w83781d_temp(struct i2c_client *client, int operation, int ctl_name,
               int *nrels_mag, long *results,int tempnum )
{
  struct w83781d_data *data = client->data;
  if (operation == SENSORS_PROC_REAL_INFO)
    *nrels_mag = 1;
  else if (operation == SENSORS_PROC_REAL_READ) {
    w83781d_update_client(client);
    results[0] = TEMP_FROM_REG(data->temp_over[tempnum]);
    results[1] = TEMP_FROM_REG(data->temp_hyst[tempnum]);
    results[2] = TEMP_FROM_REG(data->temp[tempnum]);
    *nrels_mag = 3;
  } else if (operation == SENSORS_PROC_REAL_WRITE) {
    if (tempnum >1) {
     if (*nrels_mag >= 1) {
       data->temp_over[tempnum] = TEMP_TO_REG(results[0]);
       w83781d_write_value(client,W83781D_REG_TEMP_OVER23,data->temp_over[tempnum]);
       w83781d_write_value(client,W83781D_REG_TEMP_OVER23 + 1,0);
     }
     if (*nrels_mag >= 2) {
       data->temp_hyst[tempnum] = TEMP_TO_REG(results[1]);
       w83781d_write_value(client,W83781D_REG_TEMP_HYST23,data->temp_hyst[tempnum]);
       w83781d_write_value(client,W83781D_REG_TEMP_HYST23 + 1,0);
     }
    } else {
     if (*nrels_mag >= 1) {
       data->temp_over[tempnum] = TEMP_TO_REG(results[0]);
       w83781d_write_value(client,W83781D_REG_TEMP_OVER,data->temp_over[tempnum]);
     }
     if (*nrels_mag >= 2) {
       data->temp_hyst[tempnum] = TEMP_TO_REG(results[1]);
       w83781d_write_value(client,W83781D_REG_TEMP_HYST,data->temp_hyst[tempnum]);
     }
    }
  }
}


void w83781d_temp1(struct i2c_client *client, int operation, int ctl_name,
               int *nrels_mag, long *results) {

  w83781d_write_value(client,W83781D_REG_TEMP_SEL,0x01);/* Switch Banks!! */
  w83781d_temp(client,operation,ctl_name,nrels_mag,results,1);
  w83781d_write_value(client,W83781D_REG_TEMP_SEL,0x0);/* Switch Banks!! */
}


void w83781d_temp2(struct i2c_client *client, int operation, int ctl_name,
               int *nrels_mag, long *results) {
	       
  w83781d_write_value(client,W83781D_REG_TEMP_SEL,0x02);/* Switch Banks!! */
  w83781d_temp(client,operation,ctl_name,nrels_mag,results,2);
  w83781d_write_value(client,W83781D_REG_TEMP_SEL,0x00);/* Switch Banks!! */
}


void w83781d_temp3(struct i2c_client *client, int operation, int ctl_name,
               int *nrels_mag, long *results) {
	       
  w83781d_write_value(client,W83781D_REG_TEMP_SEL,0x04);/* Switch Banks!! */
  w83781d_temp(client,operation,ctl_name,nrels_mag,results,3);
  w83781d_write_value(client,W83781D_REG_TEMP_SEL,0x00);/* Switch Banks!! */
}


void w83781d_vid(struct i2c_client *client, int operation, int ctl_name,
              int *nrels_mag, long *results)
{
  struct w83781d_data *data = client->data;
  if (operation == SENSORS_PROC_REAL_INFO)
    *nrels_mag = 2;
  else if (operation == SENSORS_PROC_REAL_READ) {
    w83781d_update_client(client);
    results[0] = VID_FROM_REG(data->vid);
    *nrels_mag = 1;
  }
}

void w83781d_alarms(struct i2c_client *client, int operation, int ctl_name,
                 int *nrels_mag, long *results)
{
  struct w83781d_data *data = client->data;
  if (operation == SENSORS_PROC_REAL_INFO)
    *nrels_mag = 0;
  else if (operation == SENSORS_PROC_REAL_READ) {
    w83781d_update_client(client);
    results[0] = ALARMS_FROM_REG(data->alarms);
    *nrels_mag = 1;
  }
}

void w83781d_fan_div(struct i2c_client *client, int operation, int ctl_name,
                  int *nrels_mag, long *results)
{
  struct w83781d_data *data = client->data;
  int old;

  if (operation == SENSORS_PROC_REAL_INFO)
    *nrels_mag = 0;
  else if (operation == SENSORS_PROC_REAL_READ) {
    w83781d_update_client(client);
    results[0] = DIV_FROM_REG(data->fan_div[0]);
    results[1] = DIV_FROM_REG(data->fan_div[1]);
    results[2] = 2;
    *nrels_mag = 3;
  } else if (operation == SENSORS_PROC_REAL_WRITE) {
    old = w83781d_read_value(client,W83781D_REG_VID_FANDIV);
    if (*nrels_mag >= 2) {
      data->fan_div[1] = DIV_TO_REG(results[1]);
      old = (old & 0x3f) | (data->fan_div[1] << 6);
    }
    if (*nrels_mag >= 1) {
      data->fan_div[0] = DIV_TO_REG(results[0]);
      old = (old & 0xcf) | (data->fan_div[0] << 4);
      w83781d_write_value(client,W83781D_REG_VID_FANDIV,old);
    }
  }
}

int w83781d_init(void)
{
  int res;

  printk("w83781d.o version %s (%s)\n",LM_VERSION,LM_DATE);
  w83781d_initialized = 0;

  if ((res =i2c_add_driver(&w83781d_driver))) {
    printk("w83781d.o: Driver registration failed, module not inserted.\n");
    w83781d_cleanup();
    return res;
  }
  w83781d_initialized ++;
  return 0;
}

int w83781d_cleanup(void)
{
  int res;

  if (w83781d_initialized >= 1) {
    if ((res = i2c_del_driver(&w83781d_driver))) {
      printk("w83781d.o: Driver deregistration failed, module not removed.\n");
      return res;
    }
    w83781d_initialized --;
  }
  return 0;
}


#ifdef MODULE

MODULE_AUTHOR("Frodo Looijaard <frodol@dds.nl> and Philip Edelbrock <phil@netroedge.com>");
MODULE_DESCRIPTION("W83781D driver");

int init_module(void)
{
  return w83781d_init();
}

int cleanup_module(void)
{
  return w83781d_cleanup();
}

#endif /* MODULE */

