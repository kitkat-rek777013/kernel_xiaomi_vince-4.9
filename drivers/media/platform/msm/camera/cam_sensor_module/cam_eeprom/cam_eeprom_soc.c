/* Copyright (c) 2017, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/of.h>
#include <linux/of_gpio.h>
#include <cam_sensor_cmn_header.h>
#include <cam_sensor_util.h>
#include <cam_sensor_io.h>
#include <cam_req_mgr_util.h>

#include "cam_eeprom_soc.h"
#include "cam_debug_util.h"

/*
 * cam_eeprom_parse_memory_map() - parse memory map in device node
 * @of:         device node
 * @data:       memory block for output
 *
 * This functions parses @of to fill @data.  It allocates map itself, parses
 * the @of node, calculate total data length, and allocates required buffer.
 * It only fills the map, but does not perform actual reading.
 */
int cam_eeprom_parse_dt_memory_map(struct device_node *node,
	struct cam_eeprom_memory_block_t *data)
{
	int       i, rc = 0;
	char      property[PROPERTY_MAXSIZE];
	uint32_t  count = MSM_EEPROM_MEM_MAP_PROPERTIES_CNT;
	struct    cam_eeprom_memory_map_t *map;

	snprintf(property, PROPERTY_MAXSIZE, "num-blocks");
	rc = of_property_read_u32(node, property, &data->num_map);
	if (rc < 0) {
		CAM_ERR(CAM_EEPROM, "failed: num-blocks not available rc %d",
			rc);
		return rc;
	}

	map = kzalloc((sizeof(*map) * data->num_map), GFP_KERNEL);
	if (!map) {
		rc = -ENOMEM;
		return rc;
	}
	data->map = map;

	for (i = 0; i < data->num_map; i++) {
		snprintf(property, PROPERTY_MAXSIZE, "page%d", i);
		rc = of_property_read_u32_array(node, property,
			(uint32_t *) &map[i].page, count);
		if (rc < 0) {
			CAM_ERR(CAM_EEPROM, "failed: page not available rc %d",
				rc);
			goto ERROR;
		}

		snprintf(property, PROPERTY_MAXSIZE, "pageen%d", i);
		rc = of_property_read_u32_array(node, property,
			(uint32_t *) &map[i].pageen, count);
		if (rc < 0)
			CAM_DBG(CAM_EEPROM, "pageen not needed");

		snprintf(property, PROPERTY_MAXSIZE, "saddr%d", i);
		rc = of_property_read_u32_array(node, property,
			(uint32_t *) &map[i].saddr, 1);
		if (rc < 0)
			CAM_DBG(CAM_EEPROM, "saddr not needed - block %d", i);

		snprintf(property, PROPERTY_MAXSIZE, "poll%d", i);
		rc = of_property_read_u32_array(node, property,
			(uint32_t *) &map[i].poll, count);
		if (rc < 0) {
			CAM_ERR(CAM_EEPROM, "failed: poll not available rc %d",
				rc);
			goto ERROR;
		}

		snprintf(property, PROPERTY_MAXSIZE, "mem%d", i);
		rc = of_property_read_u32_array(node, property,
			(uint32_t *) &map[i].mem, count);
		if (rc < 0) {
			CAM_ERR(CAM_EEPROM, "failed: mem not available rc %d",
				rc);
			goto ERROR;
		}
		data->num_data += map[i].mem.valid_size;
	}

	data->mapdata = kzalloc(data->num_data, GFP_KERNEL);
	if (!data->mapdata) {
		rc = -ENOMEM;
		goto ERROR;
	}
	return rc;

ERROR:
	kfree(data->map);
	memset(data, 0, sizeof(*data));
	return rc;
}

/**
 * @e_ctrl: ctrl structure
 *
 * Parses eeprom dt
 */
static int cam_eeprom_get_dt_data(struct cam_eeprom_ctrl_t *e_ctrl)
{
	int                             rc = 0;
	struct cam_hw_soc_info         *soc_info = &e_ctrl->soc_info;
	struct cam_eeprom_soc_private  *soc_private =
		(struct cam_eeprom_soc_private *)e_ctrl->soc_info.soc_private;
	struct cam_sensor_power_ctrl_t *power_info = &soc_private->power_info;
	struct device_node             *of_node = NULL;

	if (e_ctrl->eeprom_device_type == MSM_CAMERA_SPI_DEVICE)
		of_node = e_ctrl->io_master_info.
			spi_client->spi_master->dev.of_node;
	else if (e_ctrl->eeprom_device_type == MSM_CAMERA_PLATFORM_DEVICE)
		of_node = soc_info->pdev->dev.of_node;

	if (!of_node) {
		CAM_ERR(CAM_EEPROM, "of_node is NULL, device type %d",
			e_ctrl->eeprom_device_type);
		return -EINVAL;
	}
	rc = cam_soc_util_get_dt_properties(soc_info);
	if (rc < 0) {
		CAM_ERR(CAM_EEPROM, "cam_soc_util_get_dt_properties rc %d",
			rc);
		return rc;
	}

	if (e_ctrl->userspace_probe == false) {
		rc = cam_get_dt_power_setting_data(of_node,
			soc_info, power_info);
		if (rc < 0) {
			CAM_ERR(CAM_EEPROM, "failed in getting power settings");
			return rc;
		}
	}

	if (!soc_info->gpio_data) {
		CAM_INFO(CAM_EEPROM, "No GPIO found");
		return 0;
	}

	if (!soc_info->gpio_data->cam_gpio_common_tbl_size) {
		CAM_INFO(CAM_EEPROM, "No GPIO found");
		return -EINVAL;
	}

	rc = cam_sensor_util_init_gpio_pin_tbl(soc_info,
		&power_info->gpio_num_info);
	if ((rc < 0) || (!power_info->gpio_num_info)) {
		CAM_ERR(CAM_EEPROM, "No/Error EEPROM GPIOs");
		return -EINVAL;
	}

	return rc;
}

/**
 * @eb_info: eeprom private data structure
 * @of_node: eeprom device node
 *
 * This function parses the eeprom dt to get the MM data
 */
static int cam_eeprom_cmm_dts(struct cam_eeprom_soc_private *eb_info,
	struct device_node *of_node)
{
	int                      rc = 0;
	struct cam_eeprom_cmm_t *cmm_data = &eb_info->cmm_data;

	cmm_data->cmm_support =
		of_property_read_bool(of_node, "cmm-data-support");
	if (!cmm_data->cmm_support) {
		CAM_DBG(CAM_EEPROM, "No cmm support");
		return 0;
	}

	cmm_data->cmm_compression =
		of_property_read_bool(of_node, "cmm-data-compressed");

	rc = of_property_read_u32(of_node, "cmm-data-offset",
		&cmm_data->cmm_offset);
	if (rc < 0)
		CAM_DBG(CAM_EEPROM, "No MM offset data rc %d", rc);

	rc = of_property_read_u32(of_node, "cmm-data-size",
		&cmm_data->cmm_size);
	if (rc < 0)
		CAM_DBG(CAM_EEPROM, "No MM size data rc %d", rc);

	CAM_DBG(CAM_EEPROM, "cmm_compr %d, cmm_offset %d, cmm_size %d",
		cmm_data->cmm_compression, cmm_data->cmm_offset,
		cmm_data->cmm_size);
	return 0;
}

/**
 * @e_ctrl: ctrl structure
 *
 * This function is called from cam_eeprom_spi_driver_probe, it parses
 * the eeprom dt node and decides for userspace or kernel probe.
 */
int cam_eeprom_spi_driver_soc_init(struct cam_eeprom_ctrl_t *e_ctrl)
{
	int                             rc = 0;
	struct cam_eeprom_soc_private  *soc_private =
		(struct cam_eeprom_soc_private *)e_ctrl->soc_info.soc_private;

	rc = of_property_read_u32(e_ctrl->spi->dev.of_node, "cell-index",
		&e_ctrl->subdev_id);
	if (rc < 0) {
		CAM_ERR(CAM_EEPROM, "failed rc %d", rc);
		return rc;
	}
	rc = of_property_read_string(e_ctrl->spi->dev.of_node,
		"eeprom-name", &soc_private->eeprom_name);
	if (rc < 0) {
		CAM_ERR(CAM_EEPROM, "failed rc %d", rc);
		e_ctrl->userspace_probe = true;
	}

	CAM_DBG(CAM_EEPROM, "eeprom-name %s, rc %d", soc_private->eeprom_name,
		rc);
	rc = cam_eeprom_cmm_dts(soc_private,
		e_ctrl->io_master_info.spi_client->spi_master->dev.of_node);
	if (rc < 0)
		CAM_DBG(CAM_EEPROM, "MM data not available rc %d", rc);
	rc = cam_eeprom_get_dt_data(e_ctrl);
	if (rc < 0)
		CAM_DBG(CAM_EEPROM, "failed: eeprom get dt data rc %d", rc);

	return rc;
}

/**
 * @e_ctrl: ctrl structure
 *
 * This function is called from cam_eeprom_platform_driver_probe, it parses
 * the eeprom dt node and decides for userspace or kernel probe.
 */
int cam_eeprom_platform_driver_soc_init(struct cam_eeprom_ctrl_t *e_ctrl)
{
	int                             rc = 0;
	struct cam_hw_soc_info         *soc_info = &e_ctrl->soc_info;
	struct device_node             *of_node = NULL;
	struct platform_device         *pdev = NULL;
	struct cam_eeprom_soc_private  *soc_private =
		(struct cam_eeprom_soc_private *)e_ctrl->soc_info.soc_private;
	uint32_t                        temp;

	if (!soc_info->pdev) {
		CAM_ERR(CAM_EEPROM, "Error:soc_info is not initialized");
		return -EINVAL;
	}

	pdev = soc_info->pdev;
	of_node = pdev->dev.of_node;
	if (!of_node) {
		CAM_ERR(CAM_EEPROM, "dev.of_node NULL");
		return -EINVAL;
	}

	rc = of_property_read_u32(of_node, "cell-index",
		&e_ctrl->subdev_id);
	if (rc < 0) {
		CAM_ERR(CAM_EEPROM, "failed rc %d\n", rc);
		return rc;
	}

	rc = of_property_read_u32(of_node, "cci-master",
		&e_ctrl->cci_i2c_master);
	if (rc < 0) {
		CAM_DBG(CAM_EEPROM, "failed rc %d", rc);
		return rc;
	}

	rc = of_property_read_string(of_node, "eeprom-name",
		&soc_private->eeprom_name);
	if (rc < 0) {
		CAM_DBG(CAM_EEPROM, "kernel probe is not enabled");
		e_ctrl->userspace_probe = true;
	}

	rc = cam_eeprom_get_dt_data(e_ctrl);
	if (rc < 0)
		CAM_DBG(CAM_EEPROM, "failed: eeprom get dt data rc %d", rc);

	if (e_ctrl->userspace_probe == false) {
		rc = of_property_read_u32(of_node, "slave-addr", &temp);
		if (rc < 0)
			CAM_DBG(CAM_EEPROM, "failed: no slave-addr rc %d", rc);

		soc_private->i2c_info.slave_addr = temp;

		rc = of_property_read_u32(of_node, "i2c-freq-mode", &temp);
		soc_private->i2c_info.i2c_freq_mode = temp;
		if (rc < 0) {
			CAM_ERR(CAM_EEPROM,
				"i2c-freq-mode read fail %d", rc);
			soc_private->i2c_info.i2c_freq_mode = 0;
		}
		if (soc_private->i2c_info.i2c_freq_mode	>= I2C_MAX_MODES) {
			CAM_ERR(CAM_EEPROM, "invalid i2c_freq_mode = %d",
				soc_private->i2c_info.i2c_freq_mode);
			soc_private->i2c_info.i2c_freq_mode = 0;
		}
		CAM_DBG(CAM_EEPROM, "slave-addr = 0x%X",
			soc_private->i2c_info.slave_addr);
	}
	return rc;
}