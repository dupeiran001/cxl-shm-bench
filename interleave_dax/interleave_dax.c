// SPDX-License-Identifier: GPL-2.0
/*
 * Simple PFN-mapped /dev/interleave_dax device with weighted interleave.
 *
 * This module maps a physically contiguous window starting at base_phys
 * into userspace via mmap(). The mapping is PFN-based (VM_PFNMAP | VM_IO)
 * and uses vmf_insert_pfn_prot() in the fault handler.
 *
 * Parameters:
 *   base_phys : physical base address of the window (required)
 *   config    : "<start>-<end>:weight[,...]" in GiB, relative to base_phys
 *
 * Example:
 *   base_phys=0x3040000000
 *   config=0-128:1,128-256:1
 *
 * Semantics:
 *   - Each entry defines a physical range:
 *       [ base_phys + start*1GiB , base_phys + end*1GiB )
 *   - We convert these to page ranges (start_page, end_page).
 *   - weight is an integer stripe width (in pages) per "round" for that range.
 *   - Logical page index idx inside the VMA is mapped by:
 *
 *       total_weight = sum_i weight[i]
 *       round        = idx / total_weight
 *       off_in_round = idx % total_weight
 *
 *       Scan ranges in order, subtracting weight[i] from off_in_round,
 *       until off_in_round < weight[i]; that chooses range i and "k =
 * off_in_round".
 *
 *       pos_in_range_pages = round * weight[i] + k
 *       pfn = base_pfn + start_page[i] + pos_in_range_pages
 *
 *   - If pos_in_range_pages >= len_pages[i], we SIGBUS that page.
 *
 * Userspace:
 *   mmap(/dev/interleave_dax, size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0)
 */

#include <linux/fs.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/uaccess.h>

MODULE_DESCRIPTION(
    "PFN-mapped interleave_dax test device with weighted interleave");
MODULE_LICENSE("GPL");

/*
 * ---- Module parameters (original names retained) ----
 *
 * NOTE: The user must pass these via insmod, e.g.:
 *   insmod interleave_dax.ko base_phys=0x3040000000 \
 *         config=0-128:1,128-256:1
 */

static unsigned long base_phys;
module_param(base_phys, ulong, 0444);
MODULE_PARM_DESC(base_phys, "Physical base address for interleave window");

static char *config;
module_param(config, charp, 0444);
MODULE_PARM_DESC(config,
                 "Interleave config string: \"start-end:weight,...\" in GiB");

/*
 * ---- Device / config state ----
 */

#define IL_MAX_RANGES 32

struct il_range {
  unsigned long start_page; /* pages from base_phys */
  unsigned long end_page;   /* pages from base_phys, exclusive */
  unsigned long len_pages;  /* end_page - start_page */
  unsigned int weight;      /* pages per round for this range */
};

struct il_device {
  phys_addr_t base_phys;
  int nr_ranges;
  struct il_range ranges[IL_MAX_RANGES];
  unsigned int total_weight;
};

static struct il_device il_dev;

/*
 * Parse config string of the form:
 *   "<start>-<end>:weight[,<start>-<end>:weight...]"
 * Units for start/end: GiB relative to base_phys.
 */
static int il_parse_config(struct il_device *il) {
  char *cfg, *p, *tok;
  int rc = 0;

  if (!config || !*config) {
    pr_err("interleave_dax: config must be set (e.g. 0-128:1,128-256:1)\n");
    return -EINVAL;
  }

  cfg = kstrdup(config, GFP_KERNEL);
  if (!cfg)
    return -ENOMEM;

  p = cfg;
  while ((tok = strsep(&p, ",")) != NULL) {
    unsigned long start_gib, end_gib;
    unsigned int weight;
    char *dash, *colon;
    struct il_range *r;
    unsigned long start_page, end_page;

    /* Skip empty tokens (e.g. ",,") */
    if (!*tok)
      continue;

    dash = strchr(tok, '-');
    colon = strchr(tok, ':');
    if (!dash || !colon || dash > colon) {
      pr_err("interleave_dax: bad token '%s' in config\n", tok);
      rc = -EINVAL;
      goto out;
    }

    *dash = '\0';
    *colon = '\0';

    /* tok = start, dash+1 = end, colon+1 = weight */
    rc = kstrtoul(tok, 0, &start_gib);
    if (rc) {
      pr_err("interleave_dax: bad start '%s'\n", tok);
      goto out;
    }
    rc = kstrtoul(dash + 1, 0, &end_gib);
    if (rc) {
      pr_err("interleave_dax: bad end '%s'\n", dash + 1);
      goto out;
    }
    rc = kstrtouint(colon + 1, 0, &weight);
    if (rc) {
      pr_err("interleave_dax: bad weight '%s'\n", colon + 1);
      goto out;
    }

    if (end_gib <= start_gib || weight == 0) {
      pr_err("interleave_dax: invalid range %lu-%lu:%u (GiB)\n", start_gib,
             end_gib, weight);
      rc = -EINVAL;
      goto out;
    }

    if (il->nr_ranges >= IL_MAX_RANGES) {
      pr_err("interleave_dax: too many ranges (max %d)\n", IL_MAX_RANGES);
      rc = -EINVAL;
      goto out;
    }

    /*
     * Convert GiB to pages.
     *  start_page = (start_gib * 1GiB) / PAGE_SIZE
     *             = (start_gib << 30) >> PAGE_SHIFT
     */
    start_page = (start_gib << 30) >> PAGE_SHIFT;
    end_page = (end_gib << 30) >> PAGE_SHIFT;

    if (end_page <= start_page) {
      pr_err("interleave_dax: range too small after page conv: %lu-%lu GiB\n",
             start_gib, end_gib);
      rc = -EINVAL;
      goto out;
    }

    r = &il->ranges[il->nr_ranges++];
    r->start_page = start_page;
    r->end_page = end_page;
    r->len_pages = end_page - start_page;
    r->weight = weight;

    il->total_weight += weight;
  }

  if (il->nr_ranges == 0 || il->total_weight == 0) {
    pr_err("interleave_dax: no valid ranges parsed from config '%s'\n", config);
    rc = -EINVAL;
    goto out;
  }

  pr_info("interleave_dax: parsed %d ranges, total_weight=%u\n", il->nr_ranges,
          il->total_weight);

out:
  kfree(cfg);
  return rc;
}

/*
 * Map logical page index idx (0-based within the VMA) to a PFN using
 * the weighted GiB-based interleave pattern.
 *
 * Returns 0 on success, -ERANGE if the idx exceeds available pages,
 * or -EINVAL on internal error.
 */
static int il_idx_to_pfn(const struct il_device *il, unsigned long idx,
                         unsigned long *pfn_out) {
  unsigned long round;
  unsigned int off_in_round;
  int i;
  unsigned long base_pfn;

  if (il->nr_ranges <= 0 || il->total_weight == 0)
    return -EINVAL;

  round = div_u64(idx, il->total_weight);
  off_in_round = idx % il->total_weight;

  /* Find which range this idx falls into for this round. */
  for (i = 0; i < il->nr_ranges; i++) {
    const struct il_range *r = &il->ranges[i];

    if (off_in_round < r->weight) {
      unsigned int k = off_in_round;
      unsigned long pos_in_range = round * r->weight + k;

      if (pos_in_range >= r->len_pages) {
        /* We've exhausted this range's pages. */
        pr_debug("interleave_dax: idx %lu -> range %d exhausted (pos=%lu, "
                 "len=%lu)\n",
                 idx, i, pos_in_range, r->len_pages);
        return -ERANGE;
      }

      base_pfn = (unsigned long)(il->base_phys >> PAGE_SHIFT);
      *pfn_out = base_pfn + r->start_page + pos_in_range;
      return 0;
    }

    off_in_round -= r->weight;
  }

  /* Should be unreachable if total_weight is consistent. */
  return -EINVAL;
}

/*
 * ---- VM operations ----
 */

static vm_fault_t il_fault(struct vm_fault *vmf) {
  struct vm_area_struct *vma = vmf->vma;
  struct il_device *il = vma->vm_private_data;
  unsigned long addr = vmf->address;
  unsigned long idx;
  unsigned long pfn;
  pgprot_t prot;
  int rc;

  if (!il)
    return VM_FAULT_SIGBUS;

  if (addr < vma->vm_start || addr >= vma->vm_end)
    return VM_FAULT_SIGBUS;

  /*
   * Logical page index inside this VMA.
   *  idx = (addr - vm_start) / PAGE_SIZE
   */
  idx = vma->vm_pgoff + ((addr - vma->vm_start) >> PAGE_SHIFT);

  rc = il_idx_to_pfn(il, idx, &pfn);
  if (rc) {
    if (rc == -ERANGE) {
      pr_debug("interleave_dax: out-of-range fault idx=%lu\n", idx);
      return VM_FAULT_SIGBUS;
    }
    pr_err("interleave_dax: il_idx_to_pfn failed (rc=%d, idx=%lu)\n", rc, idx);
    return VM_FAULT_SIGBUS;
  }

  /*
   * Derive protections from vma->vm_page_prot; force RW.
   * (You can refine this if you want RO or special attributes.)
   */
  prot = __pgprot(pgprot_val(vma->vm_page_prot) | _PAGE_RW);

  return vmf_insert_pfn_prot(vma, addr, pfn, prot);
}

static const struct vm_operations_struct il_vm_ops = {
    .fault = il_fault,
};

/*
 * ---- mmap() implementation ----
 */

static int il_mmap(struct file *filp, struct vm_area_struct *vma) {
  struct il_device *il = filp->private_data;
  unsigned long size = vma->vm_end - vma->vm_start;
  unsigned long base_pfn;
  unsigned long offset_pages = vma->vm_pgoff;

  if (!il)
    return -ENODEV;

  if (size & (PAGE_SIZE - 1)) {
    pr_err("interleave_dax: mmap size must be page-aligned (got 0x%lx)\n",
           size);
    return -EINVAL;
  }

  /*
   * Base PFN for this mapping. We still set vm_pgoff to base_pfn
   * just for debugging / consistency, even though the fault handler
   * now uses il->base_phys + interleaved offsets.
   */
  base_pfn = (unsigned long)(il->base_phys >> PAGE_SHIFT);

  /*
   * Mark this as PFN-mapped I/O. vmf_insert_pfn_prot() requires
   * VM_PFNMAP | VM_IO. DONTEXPAND and DONTDUMP are standard for
   * device mappings.
   */
  vm_flags_set(vma, VM_IO | VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP);

  /* Preserve vm_pgoff (mmap() offset in pages) so per-thread offsets work. */

  /* Optional: tweak caching behavior if desired. */
  /* e.g.:
   * vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);
   */

  vma->vm_private_data = il;
  vma->vm_ops = &il_vm_ops;

  pr_info("interleave_dax: mmap %lu KiB @ phys 0x%llx (base_pfn=0x%lx, off_pages=%lu)\n",
          size >> 10, (unsigned long long)il->base_phys, base_pfn, offset_pages);

  return 0;
}

/*
 * ---- File operations ----
 */

static int il_open(struct inode *inode, struct file *filp) {
  /* Single global device for now. If you add multi-device, adjust this. */
  filp->private_data = &il_dev;
  return 0;
}

static int il_release(struct inode *inode, struct file *filp) { return 0; }

static const struct file_operations il_fops = {
    .owner = THIS_MODULE,
    .open = il_open,
    .release = il_release,
    .mmap = il_mmap,
};

/*
 * ---- Misc device registration (/dev/interleave_dax) ----
 */

static struct miscdevice il_miscdev = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = "interleave_dax",
    .fops = &il_fops,
};

/*
 * ---- Module init / exit ----
 */

static int __init il_init(void) {
  int ret;

  if (!base_phys) {
    pr_err("interleave_dax: base_phys must be set via module param\n");
    return -EINVAL;
  }

  il_dev.base_phys = (phys_addr_t)base_phys;
  il_dev.nr_ranges = 0;
  il_dev.total_weight = 0;

  ret = il_parse_config(&il_dev);
  if (ret)
    return ret;

  pr_info("interleave_dax: base_phys=0x%llx, config=%s\n",
          (unsigned long long)il_dev.base_phys, config ? config : "(none)");

  ret = misc_register(&il_miscdev);
  if (ret) {
    pr_err("interleave_dax: failed to register miscdevice: %d\n", ret);
    return ret;
  }

  pr_info("interleave_dax: registered /dev/%s\n", il_miscdev.name);
  return 0;
}

static void __exit il_exit(void) {
  misc_deregister(&il_miscdev);
  pr_info("interleave_dax: unloaded\n");
}

module_init(il_init);
module_exit(il_exit);
