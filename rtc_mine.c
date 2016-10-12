#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/device.h>
#include <linux/rtc.h>
#include <linux/platform_device.h>

#include <linux/jiffies.h>
#include <linux/time.h>
#include <linux/delay.h>
#include <linux/sched.h>

#include <asm/uaccess.h>
#include <linux/fs.h>
#include <linux/proc_fs.h>
#include <linux/slab.h>
#include <linux/kthread.h>
#include <linux/random.h>


#define MIL 1000000L

 /*  Factor which determines what value of speed means 1x.
  *  Defines in sources only.
  */
#define S_FACTOR 100
#define S_QUOT (MIL / S_FACTOR)
#define S_REMAINDER (MIL % S_FACTOR)

MODULE_LICENSE("Dual BSD/GPL");
MODULE_AUTHOR("Ivan Lobov <lobovi138@gmail.com>");

/*Parameter which stores unix secs*/
static long   time_sec = 0;
/*Parameter which stores unix usecs*/
static int    time_usec = 0;
/*Speed of time flow in that module, 100 means 1x, 75 means 0.75x etc*/
static int    speed = 100;
/*Parameter which switch random mode, in random mode speed dosn't matter*/
static int    random = 0;
/*In random mode that param describes distribution range of speed value*/
static short  random_bound = 300;

module_param(time_sec, long, S_IRUGO | S_IWUGO);
module_param(time_usec, int, S_IRUGO | S_IWUGO);
module_param(speed, int, S_IRUGO | S_IWUGO);
module_param(random, int, S_IRUGO | S_IWUGO);
module_param(random_bound, short, S_IRUGO | S_IWUGO);

static int init_time;

static int main_thread(void* data);
static int rtc_mine_init(void);
void rtc_mine_exit(void);
static int rtc_mine_probe(struct platform_device *pdev);

static int procfile_open(struct inode *inode, struct file *file);
static int procfile_show(struct seq_file *m, void *v);
static ssize_t procfile_write(struct file *, const char __user *, size_t, loff_t *);

static int read_rtc_time(struct device *dev, struct rtc_time *tm);
static int set_rtc_time(struct device *dev, struct rtc_time *tm);

#define SUCCESS 0
#define DEVICE_NAME "rtc_mine"
#define DEVICE_NAME_1 "dev_mine"
#define BUF_LEN 80
#define PROCFS_MAX_SIZE 2048
#define PROCFS_NAME     "rtcmine"

struct proc_dir_entry *proc_file;
static char procfs_buffer[PROCFS_MAX_SIZE];
static unsigned long procfs_buffer_size = 0;

static struct platform_device *rtc_mine_platform_device;

static struct platform_driver rtc_mine_platform_driver = {
  .driver  = {
    .name  = DEVICE_NAME,
    .owner = THIS_MODULE,
  },
  .probe   = rtc_mine_probe,
};

static struct file_operations proc_fops = {
  .owner   = THIS_MODULE,
  .open    = procfile_open,
  .read    = seq_read,
  .write   = procfile_write,
  .llseek  = seq_lseek,
  .release = single_release
};

static struct rtc_class_ops rtc_ops = {
  .read_time = read_rtc_time,
  .set_time = set_rtc_time,
};

struct task_struct *task;

void rtc_mine_exit(void)
{
  remove_proc_entry(PROCFS_NAME, NULL);
  platform_driver_unregister(&rtc_mine_platform_driver);

  kthread_stop(task);
  printk(KERN_ALERT "RTC_mine module was removed.\n");
}

static int rtc_mine_init(void)
{
  int err;
  struct timeval* tv;

  tv = kmalloc(sizeof(struct timeval), GFP_KERNEL);
  do_gettimeofday(tv);

  /*Init time parameters, which store seconds and useconds*/
  init_time = tv->tv_sec;
  time_sec = tv->tv_sec;
  time_usec = tv->tv_usec;

  task = kthread_run(main_thread, &tv, "main loop");
  wake_up_process(task);

  /* platform device */
  err = platform_driver_register(&rtc_mine_platform_driver);
  if(err)
    return err;

  rtc_mine_platform_device = platform_device_alloc(DEVICE_NAME, 0);
  if(rtc_mine_platform_device == NULL){
    err = -ENOMEM;
    platform_driver_unregister(&rtc_mine_platform_driver);
    return err;
  }

  err = platform_device_add(rtc_mine_platform_device);
  if(err)
    platform_device_put(rtc_mine_platform_device);

  /*Register procfs*/
  proc_file = proc_create(PROCFS_NAME, 0777, NULL, &proc_fops);
  if(proc_file == NULL) {
    remove_proc_entry(PROCFS_NAME, NULL);
    printk(KERN_ALERT "Error: could not initialize /proc/%s\n",
          PROCFS_NAME);
    return -ENOMEM;
  }

  printk(KERN_INFO "/proc/%s created\n", PROCFS_NAME);
  return SUCCESS;

}

static int rtc_mine_probe(struct platform_device *pdev)
{
  struct rtc_device *rtc;
  rtc = devm_rtc_device_register(&pdev->dev, DEVICE_NAME, &rtc_ops, THIS_MODULE);
  if(IS_ERR(rtc))
    return PTR_ERR(rtc);
  platform_set_drvdata(pdev, rtc);
  return SUCCESS;
}


static int main_thread(void* data)
{
  /*if random mode on, we compute value of speed in every iteration*/
  unsigned short rand_speed;
  while(!kthread_should_stop()){
    set_current_state(TASK_INTERRUPTIBLE);
    schedule_timeout(HZ);
    if(speed < 0){
      printk("RTC mine: Speed should be non-negative number. Thread stopped.\n");
      return -EINVAL;
    }

    if(random){
      get_random_bytes(&rand_speed, sizeof(rand_speed));
      rand_speed %= random_bound;
      time_sec += (S_QUOT * rand_speed +
          S_REMAINDER * rand_speed + time_usec) / MIL;
      time_usec = (S_QUOT * rand_speed +
          S_REMAINDER * rand_speed + time_usec) % MIL;

    } else {
      time_sec += (S_QUOT * speed +
          S_REMAINDER * speed + time_usec) / MIL;
      time_usec = (S_QUOT * speed +
          S_REMAINDER * speed + time_usec) % MIL;
    }
  }
    return 0;
}

static int procfile_show(struct seq_file *m, void *v)
{
  struct timeval* proctv;
  proctv = kmalloc(sizeof(struct timeval), GFP_KERNEL);
  do_gettimeofday(proctv);
  seq_printf(m, "Module uptime: %lds\nDifference between sys clock: %lds\n",
             (proctv->tv_sec - init_time), (proctv->tv_sec - time_sec));
  seq_printf(m, "Speed: %d\nRandom: %d\nRandom bound: %d\nSecs: %ld\nUsecs: %d\n",
      speed, random, random_bound, time_sec, time_usec);
  return 0;
}

static int procfile_open(struct inode *inode, struct file *file)
{
  return single_open(file, procfile_show, NULL);
}

static ssize_t procfile_write(struct file * file,
                          const char  * buffer,
                          size_t count,
                          loff_t * off)
{
  long rett = 0;
  /*Zeroing proc buffer*/
  memset(&procfs_buffer, 0, PROCFS_MAX_SIZE);
  /*Copy to kernel space*/
  procfs_buffer_size = min((int)count, PROCFS_MAX_SIZE);
  if(copy_from_user(procfs_buffer, buffer, procfs_buffer_size ) )
    return -EFAULT;
  procfs_buffer[strlen(procfs_buffer)] += '\0';


  /*srbtu [number]*/
  /*Second char must be a whitespace*/
  if(procfs_buffer[1] == ' '){
    /*After whitespace we have a number*/
    if(kstrtol(&procfs_buffer[2], 10, &rett)){
      return -EINVAL;
    }
    /*We dont have parameters which might be negative*/
    if(rett < 0)
      return -EINVAL;

    switch(procfs_buffer[0]){
      case 's':
        speed = rett;
        break;
      case 'r':
        if(rett)
          random = 1;
        else
          random = 0;
        break;
      case 'b':
        random_bound = rett;
        break;
      case 't':
        time_sec = rett;
        break;
      case 'u':
        time_usec = rett;
        break;
      default:
        return -EINVAL;
        break;
    }
  } else {
    return -EINVAL;
  }
  return procfs_buffer_size;
}

static int read_rtc_time(struct device * dev, struct rtc_time * tm)
{
  rtc_time_to_tm(time_sec, tm);
  return 0;
}

static int set_rtc_time(struct device * dev, struct rtc_time * tm)
{
  unsigned long time;
  rtc_tm_to_time(tm, &time);
  time_sec = time;
  return 0;
}

module_init(rtc_mine_init);
module_exit(rtc_mine_exit);
