/*
 * Copyright 2018 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 *
 */
#include <linux/list.h>
#include "amdgpu.h"
#include "amdgpu_xgmi.h"
#include "amdgpu_smu.h"
#include "amdgpu_ras.h"
#include "soc15.h"
#include "df/df_3_6_offset.h"
#include "xgmi/xgmi_4_0_0_smn.h"
#include "xgmi/xgmi_4_0_0_sh_mask.h"
#include "wafl/wafl2_4_0_0_smn.h"
#include "wafl/wafl2_4_0_0_sh_mask.h"

static DEFINE_MUTEX(xgmi_mutex);

#define AMDGPU_MAX_XGMI_HIVE			8
#define AMDGPU_MAX_XGMI_DEVICE_PER_HIVE		4

static struct amdgpu_hive_info xgmi_hives[AMDGPU_MAX_XGMI_HIVE];
static unsigned hive_count = 0;

static const int xgmi_pcs_err_status_reg_vg20[] = {
	smnXGMI0_PCS_GOPX16_PCS_ERROR_STATUS,
	smnXGMI0_PCS_GOPX16_PCS_ERROR_STATUS + 0x100000,
};

static const int wafl_pcs_err_status_reg_vg20[] = {
	smnPCS_GOPX1_0_PCS_GOPX1_PCS_ERROR_STATUS,
	smnPCS_GOPX1_0_PCS_GOPX1_PCS_ERROR_STATUS + 0x100000,
};

static const int xgmi_pcs_err_status_reg_arct[] = {
	smnXGMI0_PCS_GOPX16_PCS_ERROR_STATUS,
	smnXGMI0_PCS_GOPX16_PCS_ERROR_STATUS + 0x100000,
	smnXGMI0_PCS_GOPX16_PCS_ERROR_STATUS + 0x500000,
	smnXGMI0_PCS_GOPX16_PCS_ERROR_STATUS + 0x600000,
	smnXGMI0_PCS_GOPX16_PCS_ERROR_STATUS + 0x700000,
	smnXGMI0_PCS_GOPX16_PCS_ERROR_STATUS + 0x800000,
};

/* same as vg20*/
static const int wafl_pcs_err_status_reg_arct[] = {
	smnPCS_GOPX1_0_PCS_GOPX1_PCS_ERROR_STATUS,
	smnPCS_GOPX1_0_PCS_GOPX1_PCS_ERROR_STATUS + 0x100000,
};

static const struct amdgpu_pcs_ras_field xgmi_pcs_ras_fields[] = {
	{"XGMI PCS DataLossErr",
	 SOC15_REG_FIELD(XGMI0_PCS_GOPX16_PCS_ERROR_STATUS, DataLossErr)},
	{"XGMI PCS TrainingErr",
	 SOC15_REG_FIELD(XGMI0_PCS_GOPX16_PCS_ERROR_STATUS, TrainingErr)},
	{"XGMI PCS CRCErr",
	 SOC15_REG_FIELD(XGMI0_PCS_GOPX16_PCS_ERROR_STATUS, CRCErr)},
	{"XGMI PCS BERExceededErr",
	 SOC15_REG_FIELD(XGMI0_PCS_GOPX16_PCS_ERROR_STATUS, BERExceededErr)},
	{"XGMI PCS TxMetaDataErr",
	 SOC15_REG_FIELD(XGMI0_PCS_GOPX16_PCS_ERROR_STATUS, TxMetaDataErr)},
	{"XGMI PCS ReplayBufParityErr",
	 SOC15_REG_FIELD(XGMI0_PCS_GOPX16_PCS_ERROR_STATUS, ReplayBufParityErr)},
	{"XGMI PCS DataParityErr",
	 SOC15_REG_FIELD(XGMI0_PCS_GOPX16_PCS_ERROR_STATUS, DataParityErr)},
	{"XGMI PCS ReplayFifoOverflowErr",
	 SOC15_REG_FIELD(XGMI0_PCS_GOPX16_PCS_ERROR_STATUS, ReplayFifoOverflowErr)},
	{"XGMI PCS ReplayFifoUnderflowErr",
	 SOC15_REG_FIELD(XGMI0_PCS_GOPX16_PCS_ERROR_STATUS, ReplayFifoUnderflowErr)},
	{"XGMI PCS ElasticFifoOverflowErr",
	 SOC15_REG_FIELD(XGMI0_PCS_GOPX16_PCS_ERROR_STATUS, ElasticFifoOverflowErr)},
	{"XGMI PCS DeskewErr",
	 SOC15_REG_FIELD(XGMI0_PCS_GOPX16_PCS_ERROR_STATUS, DeskewErr)},
	{"XGMI PCS DataStartupLimitErr",
	 SOC15_REG_FIELD(XGMI0_PCS_GOPX16_PCS_ERROR_STATUS, DataStartupLimitErr)},
	{"XGMI PCS FCInitTimeoutErr",
	 SOC15_REG_FIELD(XGMI0_PCS_GOPX16_PCS_ERROR_STATUS, FCInitTimeoutErr)},
	{"XGMI PCS RecoveryTimeoutErr",
	 SOC15_REG_FIELD(XGMI0_PCS_GOPX16_PCS_ERROR_STATUS, RecoveryTimeoutErr)},
	{"XGMI PCS ReadySerialTimeoutErr",
	 SOC15_REG_FIELD(XGMI0_PCS_GOPX16_PCS_ERROR_STATUS, ReadySerialTimeoutErr)},
	{"XGMI PCS ReadySerialAttemptErr",
	 SOC15_REG_FIELD(XGMI0_PCS_GOPX16_PCS_ERROR_STATUS, ReadySerialAttemptErr)},
	{"XGMI PCS RecoveryAttemptErr",
	 SOC15_REG_FIELD(XGMI0_PCS_GOPX16_PCS_ERROR_STATUS, RecoveryAttemptErr)},
	{"XGMI PCS RecoveryRelockAttemptErr",
	 SOC15_REG_FIELD(XGMI0_PCS_GOPX16_PCS_ERROR_STATUS, RecoveryRelockAttemptErr)},
};

static const struct amdgpu_pcs_ras_field wafl_pcs_ras_fields[] = {
	{"WAFL PCS DataLossErr",
	 SOC15_REG_FIELD(PCS_GOPX1_0_PCS_GOPX1_PCS_ERROR_STATUS, DataLossErr)},
	{"WAFL PCS TrainingErr",
	 SOC15_REG_FIELD(PCS_GOPX1_0_PCS_GOPX1_PCS_ERROR_STATUS, TrainingErr)},
	{"WAFL PCS CRCErr",
	 SOC15_REG_FIELD(PCS_GOPX1_0_PCS_GOPX1_PCS_ERROR_STATUS, CRCErr)},
	{"WAFL PCS BERExceededErr",
	 SOC15_REG_FIELD(PCS_GOPX1_0_PCS_GOPX1_PCS_ERROR_STATUS, BERExceededErr)},
	{"WAFL PCS TxMetaDataErr",
	 SOC15_REG_FIELD(PCS_GOPX1_0_PCS_GOPX1_PCS_ERROR_STATUS, TxMetaDataErr)},
	{"WAFL PCS ReplayBufParityErr",
	 SOC15_REG_FIELD(PCS_GOPX1_0_PCS_GOPX1_PCS_ERROR_STATUS, ReplayBufParityErr)},
	{"WAFL PCS DataParityErr",
	 SOC15_REG_FIELD(PCS_GOPX1_0_PCS_GOPX1_PCS_ERROR_STATUS, DataParityErr)},
	{"WAFL PCS ReplayFifoOverflowErr",
	 SOC15_REG_FIELD(PCS_GOPX1_0_PCS_GOPX1_PCS_ERROR_STATUS, ReplayFifoOverflowErr)},
	{"WAFL PCS ReplayFifoUnderflowErr",
	 SOC15_REG_FIELD(PCS_GOPX1_0_PCS_GOPX1_PCS_ERROR_STATUS, ReplayFifoUnderflowErr)},
	{"WAFL PCS ElasticFifoOverflowErr",
	 SOC15_REG_FIELD(PCS_GOPX1_0_PCS_GOPX1_PCS_ERROR_STATUS, ElasticFifoOverflowErr)},
	{"WAFL PCS DeskewErr",
	 SOC15_REG_FIELD(PCS_GOPX1_0_PCS_GOPX1_PCS_ERROR_STATUS, DeskewErr)},
	{"WAFL PCS DataStartupLimitErr",
	 SOC15_REG_FIELD(PCS_GOPX1_0_PCS_GOPX1_PCS_ERROR_STATUS, DataStartupLimitErr)},
	{"WAFL PCS FCInitTimeoutErr",
	 SOC15_REG_FIELD(PCS_GOPX1_0_PCS_GOPX1_PCS_ERROR_STATUS, FCInitTimeoutErr)},
	{"WAFL PCS RecoveryTimeoutErr",
	 SOC15_REG_FIELD(PCS_GOPX1_0_PCS_GOPX1_PCS_ERROR_STATUS, RecoveryTimeoutErr)},
	{"WAFL PCS ReadySerialTimeoutErr",
	 SOC15_REG_FIELD(PCS_GOPX1_0_PCS_GOPX1_PCS_ERROR_STATUS, ReadySerialTimeoutErr)},
	{"WAFL PCS ReadySerialAttemptErr",
	 SOC15_REG_FIELD(PCS_GOPX1_0_PCS_GOPX1_PCS_ERROR_STATUS, ReadySerialAttemptErr)},
	{"WAFL PCS RecoveryAttemptErr",
	 SOC15_REG_FIELD(PCS_GOPX1_0_PCS_GOPX1_PCS_ERROR_STATUS, RecoveryAttemptErr)},
	{"WAFL PCS RecoveryRelockAttemptErr",
	 SOC15_REG_FIELD(PCS_GOPX1_0_PCS_GOPX1_PCS_ERROR_STATUS, RecoveryRelockAttemptErr)},
};

/**
 * DOC: AMDGPU XGMI Support
 *
 * XGMI is a high speed interconnect that joins multiple GPU cards
 * into a homogeneous memory space that is organized by a collective
 * hive ID and individual node IDs, both of which are 64-bit numbers.
 *
 * The file xgmi_device_id contains the unique per GPU device ID and
 * is stored in the /sys/class/drm/card${cardno}/device/ directory.
 *
 * Inside the device directory a sub-directory 'xgmi_hive_info' is
 * created which contains the hive ID and the list of nodes.
 *
 * The hive ID is stored in:
 *   /sys/class/drm/card${cardno}/device/xgmi_hive_info/xgmi_hive_id
 *
 * The node information is stored in numbered directories:
 *   /sys/class/drm/card${cardno}/device/xgmi_hive_info/node${nodeno}/xgmi_device_id
 *
 * Each device has their own xgmi_hive_info direction with a mirror
 * set of node sub-directories.
 *
 * The XGMI memory space is built by contiguously adding the power of
 * two padded VRAM space from each node to each other.
 *
 */


static ssize_t amdgpu_xgmi_show_hive_id(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct amdgpu_hive_info *hive =
			container_of(attr, struct amdgpu_hive_info, dev_attr);

	return snprintf(buf, PAGE_SIZE, "%llu\n", hive->hive_id);
}

static int amdgpu_xgmi_sysfs_create(struct amdgpu_device *adev,
				    struct amdgpu_hive_info *hive)
{
	int ret = 0;

	if (WARN_ON(hive->kobj))
		return -EINVAL;

	hive->kobj = kobject_create_and_add("xgmi_hive_info", &adev->dev->kobj);
	if (!hive->kobj) {
		dev_err(adev->dev, "XGMI: Failed to allocate sysfs entry!\n");
		return -EINVAL;
	}

	hive->dev_attr = (struct device_attribute) {
		.attr = {
			.name = "xgmi_hive_id",
			.mode = S_IRUGO,

		},
		.show = amdgpu_xgmi_show_hive_id,
	};

	ret = sysfs_create_file(hive->kobj, &hive->dev_attr.attr);
	if (ret) {
		dev_err(adev->dev, "XGMI: Failed to create device file xgmi_hive_id\n");
		kobject_del(hive->kobj);
		kobject_put(hive->kobj);
		hive->kobj = NULL;
	}

	return ret;
}

static void amdgpu_xgmi_sysfs_destroy(struct amdgpu_device *adev,
				    struct amdgpu_hive_info *hive)
{
	sysfs_remove_file(hive->kobj, &hive->dev_attr.attr);
	kobject_del(hive->kobj);
	kobject_put(hive->kobj);
	hive->kobj = NULL;
}

static ssize_t amdgpu_xgmi_show_device_id(struct device *dev,
				     struct device_attribute *attr,
				     char *buf)
{
	struct drm_device *ddev = dev_get_drvdata(dev);
	struct amdgpu_device *adev = ddev->dev_private;

	return snprintf(buf, PAGE_SIZE, "%llu\n", adev->gmc.xgmi.node_id);

}

#define AMDGPU_XGMI_SET_FICAA(o)	((o) | 0x456801)
static ssize_t amdgpu_xgmi_show_error(struct device *dev,
				      struct device_attribute *attr,
				      char *buf)
{
	struct drm_device *ddev = dev_get_drvdata(dev);
	struct amdgpu_device *adev = ddev->dev_private;
	uint32_t ficaa_pie_ctl_in, ficaa_pie_status_in;
	uint64_t fica_out;
	unsigned int error_count = 0;

	ficaa_pie_ctl_in = AMDGPU_XGMI_SET_FICAA(0x200);
	ficaa_pie_status_in = AMDGPU_XGMI_SET_FICAA(0x208);

	fica_out = adev->df.funcs->get_fica(adev, ficaa_pie_ctl_in);
	if (fica_out != 0x1f)
		pr_err("xGMI error counters not enabled!\n");

	fica_out = adev->df.funcs->get_fica(adev, ficaa_pie_status_in);

	if ((fica_out & 0xffff) == 2)
		error_count = ((fica_out >> 62) & 0x1) + (fica_out >> 63);

	adev->df.funcs->set_fica(adev, ficaa_pie_status_in, 0, 0);

	return snprintf(buf, PAGE_SIZE, "%d\n", error_count);
}


static DEVICE_ATTR(xgmi_device_id, S_IRUGO, amdgpu_xgmi_show_device_id, NULL);
static DEVICE_ATTR(xgmi_error, S_IRUGO, amdgpu_xgmi_show_error, NULL);

static int amdgpu_xgmi_sysfs_add_dev_info(struct amdgpu_device *adev,
					 struct amdgpu_hive_info *hive)
{
	int ret = 0;
	char node[10] = { 0 };

	/* Create xgmi device id file */
	ret = device_create_file(adev->dev, &dev_attr_xgmi_device_id);
	if (ret) {
		dev_err(adev->dev, "XGMI: Failed to create device file xgmi_device_id\n");
		return ret;
	}

	/* Create xgmi error file */
	ret = device_create_file(adev->dev, &dev_attr_xgmi_error);
	if (ret)
		pr_err("failed to create xgmi_error\n");


	/* Create sysfs link to hive info folder on the first device */
	if (adev != hive->adev) {
		ret = sysfs_create_link(&adev->dev->kobj, hive->kobj,
					"xgmi_hive_info");
		if (ret) {
			dev_err(adev->dev, "XGMI: Failed to create link to hive info");
			goto remove_file;
		}
	}

	sprintf(node, "node%d", hive->number_devices);
	/* Create sysfs link form the hive folder to yourself */
	ret = sysfs_create_link(hive->kobj, &adev->dev->kobj, node);
	if (ret) {
		dev_err(adev->dev, "XGMI: Failed to create link from hive info");
		goto remove_link;
	}

	goto success;


remove_link:
	sysfs_remove_link(&adev->dev->kobj, adev->ddev->unique);

remove_file:
	device_remove_file(adev->dev, &dev_attr_xgmi_device_id);

success:
	return ret;
}

static void amdgpu_xgmi_sysfs_rem_dev_info(struct amdgpu_device *adev,
					  struct amdgpu_hive_info *hive)
{
	char node[10];
	memset(node, 0, sizeof(node));

	device_remove_file(adev->dev, &dev_attr_xgmi_device_id);
	device_remove_file(adev->dev, &dev_attr_xgmi_error);

	if (adev != hive->adev)
		sysfs_remove_link(&adev->dev->kobj,"xgmi_hive_info");

	sprintf(node, "node%d", hive->number_devices);
	sysfs_remove_link(hive->kobj, node);

}



struct amdgpu_hive_info *amdgpu_get_xgmi_hive(struct amdgpu_device *adev, int lock)
{
	int i;
	struct amdgpu_hive_info *tmp;

	if (!adev->gmc.xgmi.hive_id)
		return NULL;

	mutex_lock(&xgmi_mutex);

	for (i = 0 ; i < hive_count; ++i) {
		tmp = &xgmi_hives[i];
		if (tmp->hive_id == adev->gmc.xgmi.hive_id) {
			if (lock)
				mutex_lock(&tmp->hive_lock);
			mutex_unlock(&xgmi_mutex);
			return tmp;
		}
	}
	if (i >= AMDGPU_MAX_XGMI_HIVE) {
		mutex_unlock(&xgmi_mutex);
		return NULL;
	}

	/* initialize new hive if not exist */
	tmp = &xgmi_hives[hive_count++];

	if (amdgpu_xgmi_sysfs_create(adev, tmp)) {
		mutex_unlock(&xgmi_mutex);
		return NULL;
	}

	tmp->adev = adev;
	tmp->hive_id = adev->gmc.xgmi.hive_id;
	INIT_LIST_HEAD(&tmp->device_list);
	mutex_init(&tmp->hive_lock);
	mutex_init(&tmp->reset_lock);
	task_barrier_init(&tmp->tb);

	if (lock)
		mutex_lock(&tmp->hive_lock);
	tmp->pstate = AMDGPU_XGMI_PSTATE_UNKNOWN;
	tmp->hi_req_gpu = NULL;
	/*
	 * hive pstate on boot is high in vega20 so we have to go to low
	 * pstate on after boot.
	 */
	tmp->hi_req_count = AMDGPU_MAX_XGMI_DEVICE_PER_HIVE;
	mutex_unlock(&xgmi_mutex);

	return tmp;
}

int amdgpu_xgmi_set_pstate(struct amdgpu_device *adev, int pstate)
{
	int ret = 0;
	struct amdgpu_hive_info *hive = amdgpu_get_xgmi_hive(adev, 0);
	struct amdgpu_device *request_adev = hive->hi_req_gpu ?
						hive->hi_req_gpu : adev;
	bool is_hi_req = pstate == AMDGPU_XGMI_PSTATE_MAX_VEGA20;
	bool init_low = hive->pstate == AMDGPU_XGMI_PSTATE_UNKNOWN;

	/* fw bug so temporarily disable pstate switching */
	return 0;

	if (!hive || adev->asic_type != CHIP_VEGA20)
		return 0;

	mutex_lock(&hive->hive_lock);

	if (is_hi_req)
		hive->hi_req_count++;
	else
		hive->hi_req_count--;

	/*
	 * Vega20 only needs single peer to request pstate high for the hive to
	 * go high but all peers must request pstate low for the hive to go low
	 */
	if (hive->pstate == pstate ||
			(!is_hi_req && hive->hi_req_count && !init_low))
		goto out;

	dev_dbg(request_adev->dev, "Set xgmi pstate %d.\n", pstate);

	ret = amdgpu_dpm_set_xgmi_pstate(request_adev, pstate);
	if (ret) {
		dev_err(request_adev->dev,
			"XGMI: Set pstate failure on device %llx, hive %llx, ret %d",
			request_adev->gmc.xgmi.node_id,
			request_adev->gmc.xgmi.hive_id, ret);
		goto out;
	}

	if (init_low)
		hive->pstate = hive->hi_req_count ?
					hive->pstate : AMDGPU_XGMI_PSTATE_MIN;
	else {
		hive->pstate = pstate;
		hive->hi_req_gpu = pstate != AMDGPU_XGMI_PSTATE_MIN ?
							adev : NULL;
	}
out:
	mutex_unlock(&hive->hive_lock);
	return ret;
}

int amdgpu_xgmi_update_topology(struct amdgpu_hive_info *hive, struct amdgpu_device *adev)
{
	int ret;

	/* Each psp need to set the latest topology */
	ret = psp_xgmi_set_topology_info(&adev->psp,
					 hive->number_devices,
					 &adev->psp.xgmi_context.top_info);
	if (ret)
		dev_err(adev->dev,
			"XGMI: Set topology failure on device %llx, hive %llx, ret %d",
			adev->gmc.xgmi.node_id,
			adev->gmc.xgmi.hive_id, ret);

	return ret;
}


int amdgpu_xgmi_get_hops_count(struct amdgpu_device *adev,
		struct amdgpu_device *peer_adev)
{
	struct psp_xgmi_topology_info *top = &adev->psp.xgmi_context.top_info;
	int i;

	for (i = 0 ; i < top->num_nodes; ++i)
		if (top->nodes[i].node_id == peer_adev->gmc.xgmi.node_id)
			return top->nodes[i].num_hops;
	return	-EINVAL;
}

int amdgpu_xgmi_add_device(struct amdgpu_device *adev)
{
	struct psp_xgmi_topology_info *top_info;
	struct amdgpu_hive_info *hive;
	struct amdgpu_xgmi	*entry;
	struct amdgpu_device *tmp_adev = NULL;

	int count = 0, ret = 0;

	if (!adev->gmc.xgmi.supported)
		return 0;

	if (amdgpu_device_ip_get_ip_block(adev, AMD_IP_BLOCK_TYPE_PSP)) {
		ret = psp_xgmi_initialize(&adev->psp);
		if (ret) {
			dev_err(adev->dev,
				"XGMI: Failed to initialize xgmi session\n");
			return ret;
		}

		ret = psp_xgmi_get_hive_id(&adev->psp, &adev->gmc.xgmi.hive_id);
		if (ret) {
			dev_err(adev->dev,
				"XGMI: Failed to get hive id\n");
			return ret;
		}

		ret = psp_xgmi_get_node_id(&adev->psp, &adev->gmc.xgmi.node_id);
		if (ret) {
			dev_err(adev->dev,
				"XGMI: Failed to get node id\n");
			return ret;
		}
	} else {
		adev->gmc.xgmi.hive_id = 16;
		adev->gmc.xgmi.node_id = adev->gmc.xgmi.physical_node_id + 16;
	}

	hive = amdgpu_get_xgmi_hive(adev, 1);
	if (!hive) {
		ret = -EINVAL;
		dev_err(adev->dev,
			"XGMI: node 0x%llx, can not match hive 0x%llx in the hive list.\n",
			adev->gmc.xgmi.node_id, adev->gmc.xgmi.hive_id);
		goto exit;
	}

	top_info = &adev->psp.xgmi_context.top_info;

	list_add_tail(&adev->gmc.xgmi.head, &hive->device_list);
	list_for_each_entry(entry, &hive->device_list, head)
		top_info->nodes[count++].node_id = entry->node_id;
	top_info->num_nodes = count;
	hive->number_devices = count;

	task_barrier_add_task(&hive->tb);

	if (amdgpu_device_ip_get_ip_block(adev, AMD_IP_BLOCK_TYPE_PSP)) {
		list_for_each_entry(tmp_adev, &hive->device_list, gmc.xgmi.head) {
			/* update node list for other device in the hive */
			if (tmp_adev != adev) {
				top_info = &tmp_adev->psp.xgmi_context.top_info;
				top_info->nodes[count - 1].node_id =
					adev->gmc.xgmi.node_id;
				top_info->num_nodes = count;
			}
			ret = amdgpu_xgmi_update_topology(hive, tmp_adev);
			if (ret)
				goto exit_unlock;
		}

		/* get latest topology info for each device from psp */
		list_for_each_entry(tmp_adev, &hive->device_list, gmc.xgmi.head) {
			ret = psp_xgmi_get_topology_info(&tmp_adev->psp, count,
					&tmp_adev->psp.xgmi_context.top_info);
			if (ret) {
				dev_err(tmp_adev->dev,
					"XGMI: Get topology failure on device %llx, hive %llx, ret %d",
					tmp_adev->gmc.xgmi.node_id,
					tmp_adev->gmc.xgmi.hive_id, ret);
				/* To do : continue with some node failed or disable the whole hive */
				goto exit_unlock;
			}
		}
	}

	if (!ret)
		ret = amdgpu_xgmi_sysfs_add_dev_info(adev, hive);

exit_unlock:
	mutex_unlock(&hive->hive_lock);
exit:
	if (!ret)
		dev_info(adev->dev, "XGMI: Add node %d, hive 0x%llx.\n",
			 adev->gmc.xgmi.physical_node_id, adev->gmc.xgmi.hive_id);
	else
		dev_err(adev->dev, "XGMI: Failed to add node %d, hive 0x%llx ret: %d\n",
			adev->gmc.xgmi.physical_node_id, adev->gmc.xgmi.hive_id,
			ret);

	return ret;
}

int amdgpu_xgmi_remove_device(struct amdgpu_device *adev)
{
	struct amdgpu_hive_info *hive;

	if (!adev->gmc.xgmi.supported)
		return -EINVAL;

	hive = amdgpu_get_xgmi_hive(adev, 1);
	if (!hive)
		return -EINVAL;

	task_barrier_rem_task(&hive->tb);
	amdgpu_xgmi_sysfs_rem_dev_info(adev, hive);
	mutex_unlock(&hive->hive_lock);

	if(!(--hive->number_devices)){
		amdgpu_xgmi_sysfs_destroy(adev, hive);
		mutex_destroy(&hive->hive_lock);
		mutex_destroy(&hive->reset_lock);
	}

	return psp_xgmi_terminate(&adev->psp);
}

int amdgpu_xgmi_ras_late_init(struct amdgpu_device *adev)
{
	int r;
	struct ras_ih_if ih_info = {
		.cb = NULL,
	};
	struct ras_fs_if fs_info = {
		.sysfs_name = "xgmi_wafl_err_count",
	};

	if (!adev->gmc.xgmi.supported ||
	    adev->gmc.xgmi.num_physical_nodes == 0)
		return 0;

	amdgpu_xgmi_reset_ras_error_count(adev);

	if (!adev->gmc.xgmi.ras_if) {
		adev->gmc.xgmi.ras_if = kmalloc(sizeof(struct ras_common_if), GFP_KERNEL);
		if (!adev->gmc.xgmi.ras_if)
			return -ENOMEM;
		adev->gmc.xgmi.ras_if->block = AMDGPU_RAS_BLOCK__XGMI_WAFL;
		adev->gmc.xgmi.ras_if->type = AMDGPU_RAS_ERROR__MULTI_UNCORRECTABLE;
		adev->gmc.xgmi.ras_if->sub_block_index = 0;
		strcpy(adev->gmc.xgmi.ras_if->name, "xgmi_wafl");
	}
	ih_info.head = fs_info.head = *adev->gmc.xgmi.ras_if;
	r = amdgpu_ras_late_init(adev, adev->gmc.xgmi.ras_if,
				 &fs_info, &ih_info);
	if (r || !amdgpu_ras_is_supported(adev, adev->gmc.xgmi.ras_if->block)) {
		kfree(adev->gmc.xgmi.ras_if);
		adev->gmc.xgmi.ras_if = NULL;
	}

	return r;
}

void amdgpu_xgmi_ras_fini(struct amdgpu_device *adev)
{
	if (amdgpu_ras_is_supported(adev, AMDGPU_RAS_BLOCK__XGMI_WAFL) &&
			adev->gmc.xgmi.ras_if) {
		struct ras_common_if *ras_if = adev->gmc.xgmi.ras_if;
		struct ras_ih_if ih_info = {
			.cb = NULL,
		};

		amdgpu_ras_late_fini(adev, ras_if, &ih_info);
		kfree(ras_if);
	}
}

uint64_t amdgpu_xgmi_get_relative_phy_addr(struct amdgpu_device *adev,
					   uint64_t addr)
{
	struct amdgpu_xgmi *xgmi = &adev->gmc.xgmi;
	return (addr + xgmi->physical_node_id * xgmi->node_segment_size);
}

static void pcs_clear_status(struct amdgpu_device *adev, uint32_t pcs_status_reg)
{
	WREG32_PCIE(pcs_status_reg, 0xFFFFFFFF);
	WREG32_PCIE(pcs_status_reg, 0);
}

void amdgpu_xgmi_reset_ras_error_count(struct amdgpu_device *adev)
{
	uint32_t i;

	switch (adev->asic_type) {
	case CHIP_ARCTURUS:
		for (i = 0; i < ARRAY_SIZE(xgmi_pcs_err_status_reg_arct); i++)
			pcs_clear_status(adev,
					 xgmi_pcs_err_status_reg_arct[i]);
		break;
	case CHIP_VEGA20:
		for (i = 0; i < ARRAY_SIZE(xgmi_pcs_err_status_reg_vg20); i++)
			pcs_clear_status(adev,
					 xgmi_pcs_err_status_reg_vg20[i]);
		break;
	default:
		break;
	}
}

static int amdgpu_xgmi_query_pcs_error_status(struct amdgpu_device *adev,
					      uint32_t value,
					      uint32_t *ue_count,
					      uint32_t *ce_count,
					      bool is_xgmi_pcs)
{
	int i;
	int ue_cnt;

	if (is_xgmi_pcs) {
		/* query xgmi pcs error status,
		 * only ue is supported */
		for (i = 0; i < ARRAY_SIZE(xgmi_pcs_ras_fields); i ++) {
			ue_cnt = (value &
				  xgmi_pcs_ras_fields[i].pcs_err_mask) >>
				  xgmi_pcs_ras_fields[i].pcs_err_shift;
			if (ue_cnt) {
				dev_info(adev->dev, "%s detected\n",
					 xgmi_pcs_ras_fields[i].err_name);
				*ue_count += ue_cnt;
			}
		}
	} else {
		/* query wafl pcs error status,
		 * only ue is supported */
		for (i = 0; i < ARRAY_SIZE(wafl_pcs_ras_fields); i++) {
			ue_cnt = (value &
				  wafl_pcs_ras_fields[i].pcs_err_mask) >>
				  wafl_pcs_ras_fields[i].pcs_err_shift;
			if (ue_cnt) {
				dev_info(adev->dev, "%s detected\n",
					 wafl_pcs_ras_fields[i].err_name);
				*ue_count += ue_cnt;
			}
		}
	}

	return 0;
}

int amdgpu_xgmi_query_ras_error_count(struct amdgpu_device *adev,
				      void *ras_error_status)
{
	struct ras_err_data *err_data = (struct ras_err_data *)ras_error_status;
	int i;
	uint32_t data;
	uint32_t ue_cnt = 0, ce_cnt = 0;

	if (!amdgpu_ras_is_supported(adev, AMDGPU_RAS_BLOCK__XGMI_WAFL))
		return -EINVAL;

	err_data->ue_count = 0;
	err_data->ce_count = 0;

	switch (adev->asic_type) {
	case CHIP_ARCTURUS:
		/* check xgmi pcs error */
		for (i = 0; i < ARRAY_SIZE(xgmi_pcs_err_status_reg_arct); i++) {
			data = RREG32_PCIE(xgmi_pcs_err_status_reg_arct[i]);
			if (data)
				amdgpu_xgmi_query_pcs_error_status(adev,
						data, &ue_cnt, &ce_cnt, true);
		}
		/* check wafl pcs error */
		for (i = 0; i < ARRAY_SIZE(wafl_pcs_err_status_reg_arct); i++) {
			data = RREG32_PCIE(wafl_pcs_err_status_reg_arct[i]);
			if (data)
				amdgpu_xgmi_query_pcs_error_status(adev,
						data, &ue_cnt, &ce_cnt, false);
		}
		break;
	case CHIP_VEGA20:
	default:
		/* check xgmi pcs error */
		for (i = 0; i < ARRAY_SIZE(xgmi_pcs_err_status_reg_vg20); i++) {
			data = RREG32_PCIE(xgmi_pcs_err_status_reg_vg20[i]);
			if (data)
				amdgpu_xgmi_query_pcs_error_status(adev,
						data, &ue_cnt, &ce_cnt, true);
		}
		/* check wafl pcs error */
		for (i = 0; i < ARRAY_SIZE(wafl_pcs_err_status_reg_vg20); i++) {
			data = RREG32_PCIE(wafl_pcs_err_status_reg_vg20[i]);
			if (data)
				amdgpu_xgmi_query_pcs_error_status(adev,
						data, &ue_cnt, &ce_cnt, false);
		}
		break;
	}

	amdgpu_xgmi_reset_ras_error_count(adev);

	err_data->ue_count += ue_cnt;
	err_data->ce_count += ce_cnt;

	return 0;
}
