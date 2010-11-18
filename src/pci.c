/*
 *  Copyright (c) 2006-2010 Dell, Inc.
 *  by Matt Domsch <Matt_Domsch@dell.com>
 *  Licensed under the GNU General Public license, version 2.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <limits.h>
#include <unistd.h>
#include <pci/pci.h>
#include "pirq.h"
#include "pci.h"
#include "sysfs.h"

static int read_pci_sysfs_path(char *buf, size_t bufsize, const struct pci_dev *pdev)
{
	char path[PATH_MAX];
	char pci_name[16];
	ssize_t size;
	unparse_pci_name(pci_name, sizeof(pci_name), pdev);
	snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s", pci_name);
	size = readlink(path, buf, bufsize);
	if (size == -1)
		return 1;
	return 0;
}

static int read_pci_sysfs_physfn(char *buf, size_t bufsize, const struct pci_dev *pdev)
{
	char path[PATH_MAX];
	char pci_name[16];
	ssize_t size;
	unparse_pci_name(pci_name, sizeof(pci_name), pdev);
	snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/physfn", pci_name);
	size = readlink(path, buf, bufsize);
	if (size == -1)
		return 1;
	return 0;
}

static int parse_pci_name(const char *s, int *domain, int *bus, int *dev, int *func)
{
	int err;
/* The domain part was added in 2.6 kernels.  Test for that first. */
	err = sscanf(s, "%x:%2x:%2x.%x", domain, bus, dev, func);
	if (err != 4) {
		err = sscanf(s, "%2x:%2x.%x", bus, dev, func);
		if (err != 3) {
			return 1;
		}
	}
	return 0;
}

static struct pci_dev * find_pdev_by_pci_name(struct pci_access *pacc, const char *s)
{
	int domain=0, bus=0, device=0, func=0;
	if (parse_pci_name(s, &domain, &bus, &device, &func))
		return NULL;
	return pci_get_dev(pacc, domain, bus, device, func);
}

static struct pci_dev *
find_physfn(struct pci_access *pacc, struct pci_dev *p)
{
	int rc;
	char path[PATH_MAX];
	char *c;
	struct pci_dev *pdev;
	memset(path, 0, sizeof(path));
	rc = read_pci_sysfs_physfn(path, sizeof(path), p);
	if (rc != 0)
		return NULL;
	/* we get back a string like
	   ../0000:05:0.0
	   where the last component is the parent device
	*/
	/* find the last backslash */
	c = rindex(path, '/');
	c++;
	pdev = find_pdev_by_pci_name(pacc, c);
	return pdev;
}

static int is_same_pci(const struct pci_dev *a, const struct pci_dev *b)
{
	if (pci_domain_nr(a) == pci_domain_nr(b) &&
	    a->bus == b->bus &&
	    a->dev == b->dev &&
	    a->func == b->func)
		return 1;
	return 0;
}

static void add_vf_to_pf(struct pci_access *pacc, struct pci_device *pf, struct pci_device *vf)
{
	struct pci_dev *pfdev;
	pfdev = find_physfn(pacc, &vf->pci_dev);

	if (!pfdev)
		return;
	vf->is_virtual_function=1;
	if (is_same_pci(&pf->pci_dev, pfdev)) {
		list_add_tail(&vf->vfnode, &pf->vfs);
		vf->vf_index = pf->num_vfs;
		pf->num_vfs++;
		vf->pf = pf;
	}
}

static struct pci_dev *
find_parent(struct pci_access *pacc, struct pci_dev *p)
{
	int rc;
	char path[PATH_MAX];
	char *c;
	struct pci_dev *pdev;
	memset(path, 0, sizeof(path));
	/* if this device has a physfn pointer, then treat _that_ as the parent */
	pdev = find_physfn(pacc, p);
	if (pdev)
		return pdev;

	rc = read_pci_sysfs_path(path, sizeof(path), p);
	if (rc != 0)
		return NULL;
	/* we get back a string like
	   ../../../devices/pci0000:00/0000:00:09.0/0000:05:17.4
	   where the last component is the device we asked for
	*/
	/* find the last backslash */
	c = rindex(path, '/');
	*c = '\0';
	/* find the last backslash again */
	c = rindex(path, '/');
	c++;
	pdev = find_pdev_by_pci_name(pacc, c);
	return pdev;
}

/*
 * Check our parents in case the device itself isn't listed
 * in the PCI IRQ Routing Table.  This has a problem, as
 * our parent bridge on a card may not be included
 * in the $PIR table.  In that case, it falls back to "unknown".
 */
static int pci_dev_to_slot(struct routing_table *table, struct pci_access *pacc, struct pci_dev *p)
{
	int rc = INT_MAX;
	rc = pirq_pci_dev_to_slot(table, p->bus, p->dev);
	while (rc == INT_MAX) {
		p = find_parent(pacc, p);
		if (p == NULL)
			break;
		rc = pirq_pci_dev_to_slot(table, p->bus, p->dev);
	}
	return rc;
}

static char *read_pci_sysfs_label(const struct pci_dev *pdev)
{
	char path[PATH_MAX];
	char pci_name[16];
	int rc;
	char *label = NULL;

	unparse_pci_name(pci_name, sizeof(pci_name), pdev);
	snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/label", pci_name);
	rc = sysfs_read_file(path, &label);
	if (rc == 0)
		return label;
	return NULL;
}

static int read_pci_sysfs_index(unsigned int *index, const struct pci_dev *pdev)
{
	char path[PATH_MAX];
	char pci_name[16];
	int rc;
	char *indexstr = NULL;
	unsigned int i;
	unparse_pci_name(pci_name, sizeof(pci_name), pdev);
	snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/index", pci_name);
	rc = sysfs_read_file(path, &indexstr);
	if (rc == 0) {
		rc = sscanf(indexstr, "%u", &i);
		if (rc == 1)  {
			*index = i;
			return 0;
		}
	}
	return 1;
}

static void fill_pci_dev_sysfs(struct pci_device *dev, struct pci_dev *p)
{
	int rc;
	unsigned int index = 0;
	char *label = NULL;
	char buf[PATH_MAX];
	unparse_pci_name(buf, sizeof(buf), p);
	rc = read_pci_sysfs_index(&index, p);
	if (!rc) {
		dev->sysfs_index = index;
		dev->uses_sysfs |= HAS_SYSFS_INDEX;
	}
	label = read_pci_sysfs_label(p);
	if (label) {
		dev->sysfs_label = label;
		dev->uses_sysfs |= HAS_SYSFS_LABEL;
	}
}

static void add_pci_dev(struct libbiosdevname_state *state,
			struct routing_table *table,
			struct pci_access *pacc,
			struct pci_dev *p)
{
	struct pci_device *dev;
	dev = malloc(sizeof(*dev));
	if (!dev) {
		fprintf(stderr, "out of memory\n");
		return;
	}
	memset(dev, 0, sizeof(*dev));
	INIT_LIST_HEAD(&dev->node);
	INIT_LIST_HEAD(&dev->vfnode);
	INIT_LIST_HEAD(&dev->vfs);
	memcpy(&dev->pci_dev, p, sizeof(*p)); /* This doesn't allow us to call PCI functions though */
	dev->physical_slot = PHYSICAL_SLOT_UNKNOWN;
	if (table)
		dev->physical_slot = pci_dev_to_slot(table, pacc, p);
	dev->class         = pci_read_word(p, PCI_CLASS_DEVICE);
	fill_pci_dev_sysfs(dev, p);
	list_add(&dev->node, &state->pci_devices);
}

void free_pci_devices(struct libbiosdevname_state *state)
{
	struct pci_device *pos, *next;
	list_for_each_entry_safe(pos, next, &state->pci_devices, node) {
		if (pos->smbios_label)
			free(pos->smbios_label);
		if (pos->sysfs_label)
			free(pos->sysfs_label);
		list_del(&pos->node);
		free(pos);
	}
}

int get_pci_devices(struct libbiosdevname_state *state)
{
	struct pci_access *pacc;
	struct pci_dev *p;
	struct pci_device *dev, *pfdev, *vfdev;
	struct routing_table *table;
	int rc=0;

	pacc = pci_alloc();
	if (!pacc)
		return rc;

	pci_init(pacc);
	pci_scan_bus(pacc);

	table = pirq_alloc_read_table();

	for (p=pacc->devices; p; p=p->next) {
		dev = find_dev_by_pci(state, p);
		if (!dev)
			add_pci_dev(state, table, pacc, p);
	}

	/* in a second pass, attach VFs to PFs */
	list_for_each_entry(pfdev, &state->pci_devices, node) {
		list_for_each_entry(vfdev, &state->pci_devices, node) {
			add_vf_to_pf(pacc, pfdev, vfdev);
		}
	}

	pirq_free_table(table);
	pci_cleanup(pacc);
	return rc;
}

int unparse_pci_name(char *buf, int size, const struct pci_dev *pdev)
{
	return snprintf(buf, size, "%04x:%02x:%02x.%x",
			pci_domain_nr(pdev), pdev->bus, pdev->dev, pdev->func);
}

static int unparse_location(char *buf, const int size, const int location)
{
	char *s = buf;
	if (location == 0)
		s += snprintf(s, size-(s-buf), "embedded");
	else if (location == INT_MAX)
		s += snprintf(s, size-(s-buf), "unknown");
	else if (location > 0)
		s += snprintf(s, size-(s-buf), "%u", location);
	else
		s += snprintf(s, size-(s-buf), "unknown");
	return (s-buf);
}

static int unparse_smbios_type41_type(char *buf, const int size, const int type)
{
	char *s = buf;
	const char *msg[] = {"Other",
			     "Unknown",
			     "Video",
			     "SCSI Controller",
			     "Ethernet",
			     "Token Ring",
			     "Sound",
			     "PATA Controller",
			     "SATA Controller",
			     "SAS Controller",
	};
	if (type > 0 && type <= sizeof(msg))
		s += snprintf(s, size-(s-buf), "%s\n", msg[type-1]);
	else
		s += snprintf(s, size-(s-buf), "<OUT OF SPEC>\n");
	return (s-buf);
}


int unparse_pci_device(char *buf, const int size, const struct pci_device *p)
{
	char *s = buf;
	struct pci_device *dev;
	char pci_name[16];
	s += snprintf(s, size-(s-buf), "PCI name      : ");
	s += unparse_pci_name(s,  size-(s-buf), &p->pci_dev);
	s += snprintf(s, size-(s-buf), "\n");
	s += snprintf(s, size-(s-buf), "PCI Slot      : ");
	if (p->physical_slot < INT_MAX)
		s += unparse_location(s, size-(s-buf), p->physical_slot);
	else
		s += snprintf(s, size-(s-buf), "Unknown");
	s += snprintf(s, size-(s-buf), "\n");
	if (p->smbios_type) {
		s += snprintf(s, size-(s-buf), "SMBIOS Device Type: ");
		s += unparse_smbios_type41_type(s, size-(s-buf), p->smbios_type);
		s += snprintf(s, size-(s-buf), "SMBIOS Instance: %u\n", p->smbios_instance);
		s += snprintf(s, size-(s-buf), "SMBIOS Enabled: %s\n", p->smbios_instance?"True":"False");
	}
	if (p->smbios_label)
		s += snprintf(s, size-(s-buf), "SMBIOS Label: %s\n", p->smbios_label);
	if (p->uses_sysfs & HAS_SYSFS_INDEX)
		s += snprintf(s, size-(s-buf), "sysfs Index: %u\n", p->sysfs_index);
	if (p->uses_sysfs & HAS_SYSFS_LABEL)
		s += snprintf(s, size-(s-buf), "sysfs Label: %s\n", p->sysfs_label);
	s += snprintf(s, size-(s-buf), "Index in slot: %u\n", p->index_in_slot);

	if (!list_empty(&p->vfs)) {
		s += snprintf(s, size-(s-buf), "Virtual Functions:\n");
		list_for_each_entry(dev, &p->vfs, vfnode) {
			unparse_pci_name(pci_name, sizeof(pci_name), &dev->pci_dev);
			s += snprintf(s, size-(s-buf), "%s\n", pci_name);
		}
	}

	return (s-buf);
}

struct pci_device * find_dev_by_pci(const struct libbiosdevname_state *state,
				    const struct pci_dev *p)
{
	struct pci_device *dev;
	list_for_each_entry(dev, &state->pci_devices, node) {
		if (is_same_pci(p, &dev->pci_dev))
			return dev;
	}
	return NULL;
}

static struct pci_device * find_pci_device_by_pci_dev(const struct libbiosdevname_state *state, struct pci_dev *p)
{
	struct pci_device *dev;

	list_for_each_entry(dev, &state->pci_devices, node) {
		if (is_same_pci(p, &dev->pci_dev))
			return dev;
	}
	return NULL;
}
struct pci_device * find_pci_dev_by_pci_addr(const struct libbiosdevname_state *state,
					     const int domain, const int bus, const int device, const int func)
{
	struct pci_device *dev;
	struct pci_device p;

#ifdef HAVE_STRUCT_PCI_DEV_DOMAIN
	p.pci_dev.domain = domain;
#endif
	p.pci_dev.bus = bus;
	p.pci_dev.dev = device;
	p.pci_dev.func = func;

	list_for_each_entry(dev, &state->pci_devices, node) {
		if (is_same_pci(&p.pci_dev, &dev->pci_dev))
			return dev;
	}
	return NULL;
}

struct pci_device * find_dev_by_pci_name(const struct libbiosdevname_state *state,
					 const char *s)
{
	int domain=0, bus=0, device=0, func=0;
	if (parse_pci_name(s, &domain, &bus, &device, &func))
		return NULL;

	return find_pci_dev_by_pci_addr(state, domain, bus, device, func);
}
