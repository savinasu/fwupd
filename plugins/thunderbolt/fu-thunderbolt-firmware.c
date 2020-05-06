/*
 * Copyright (C) 2017 Intel Corporation.
 * Copyright (C) 2020 Richard Hughes <richard@hughsie.com>
 *
 * SPDX-License-Identifier: LGPL-2.1+
 */

#include "config.h"

#include <libfwupd/fwupd-error.h>

#include "fu-common.h"
#include "fu-thunderbolt-firmware.h"

typedef enum {
	_SECTION_DIGITAL,
	_SECTION_DROM,
	_SECTION_ARC_PARAMS,
	_SECTION_DRAM_UCODE,
	_SECTION_LAST
} FuThunderboltSection;

typedef enum {
	_FAMILY_UNKNOWN,
	_FAMILY_FR,
	_FAMILY_WR,
	_FAMILY_AR,
	_FAMILY_AR_C,
	_FAMILY_TR,
	_FAMILY_BB,
} FuThunderboltFamily;

struct _FuThunderboltFirmware {
	FuFirmwareClass			 parent_instance;
	guint32				 sections[_SECTION_LAST];
	FuThunderboltFamily		 family;
	gboolean			 is_host;
	gboolean			 is_native;
	gboolean			 has_pd;
	guint16				 device_id;
	guint16				 vendor_id;
	guint16				 model_id;
	guint				 gen;
	guint				 ports;
	guint8				 flash_size;
};

G_DEFINE_TYPE (FuThunderboltFirmware, fu_thunderbolt_firmware, FU_TYPE_FIRMWARE)

typedef struct {
	guint16				 id;
	guint				 gen;
	FuThunderboltFamily		 family;
	guint				 ports;
} FuThunderboltHwInfo;

enum {
	DROM_ENTRY_MC = 0x6,
};

gboolean
fu_thunderbolt_firmware_is_host (FuThunderboltFirmware *self)
{
	g_return_val_if_fail (FU_IS_THUNDERBOLT_FIRMWARE (self), FALSE);
	return self->is_host;
}

gboolean
fu_thunderbolt_firmware_is_native (FuThunderboltFirmware *self)
{
	g_return_val_if_fail (FU_IS_THUNDERBOLT_FIRMWARE (self), FALSE);
	return self->is_native;
}

gboolean
fu_thunderbolt_firmware_get_has_pd (FuThunderboltFirmware *self)
{
	g_return_val_if_fail (FU_IS_THUNDERBOLT_FIRMWARE (self), FALSE);
	return self->has_pd;
}

guint16
fu_thunderbolt_firmware_get_device_id (FuThunderboltFirmware *self)
{
	g_return_val_if_fail (FU_IS_THUNDERBOLT_FIRMWARE (self), 0);
	return self->device_id;
}

guint16
fu_thunderbolt_firmware_get_vendor_id (FuThunderboltFirmware *self)
{
	g_return_val_if_fail (FU_IS_THUNDERBOLT_FIRMWARE (self), 0);
	return self->vendor_id;
}

guint16
fu_thunderbolt_firmware_get_model_id (FuThunderboltFirmware *self)
{
	g_return_val_if_fail (FU_IS_THUNDERBOLT_FIRMWARE (self), 0);
	return self->model_id;
}

guint8
fu_thunderbolt_firmware_get_flash_size (FuThunderboltFirmware *self)
{
	g_return_val_if_fail (FU_IS_THUNDERBOLT_FIRMWARE (self), 0);
	return self->flash_size;
}

static const gchar *
fu_thunderbolt_firmware_family_to_string (FuThunderboltFamily family)
{
	if (family == _FAMILY_FR)
		return "Falcon Ridge";
	if (family == _FAMILY_WR)
		return "Win Ridge";
	if (family == _FAMILY_AR)
		return "Alpine Ridge";
	if (family == _FAMILY_AR_C)
		return "Alpine Ridge C";
	if (family == _FAMILY_TR)
		return "Titan Ridge";
	if (family == _FAMILY_BB)
		return "BB";
	return "Unknown";
}

static void
fu_thunderbolt_firmware_to_string (FuFirmware *firmware, guint idt, GString *str)
{
	FuThunderboltFirmware *self = FU_THUNDERBOLT_FIRMWARE (firmware);
	fu_common_string_append_kv (str, idt, "Family",
				    fu_thunderbolt_firmware_family_to_string (self->family));
	fu_common_string_append_kb (str, idt, "IsHost", self->is_host);
	fu_common_string_append_kb (str, idt, "IsNative", self->is_native);
	fu_common_string_append_kx (str, idt, "DeviceId", self->device_id);
	fu_common_string_append_kx (str, idt, "VendorId", self->vendor_id);
	fu_common_string_append_kx (str, idt, "ModelId", self->model_id);
	fu_common_string_append_kx (str, idt, "FlashSize", self->flash_size);
	fu_common_string_append_kx (str, idt, "Generation", self->gen);
	fu_common_string_append_kx (str, idt, "Ports", self->ports);
	fu_common_string_append_kb (str, idt, "HasPd", self->has_pd);
	for (guint i = 0; i < _SECTION_LAST; i++) {
		g_autofree gchar *title = g_strdup_printf ("Section%u", i);
		fu_common_string_append_kx (str, idt, title, self->sections[i]);
	}
}

static inline gboolean
fu_thunderbolt_firmware_valid_farb_pointer (guint32 pointer)
{
	return pointer != 0 && pointer != 0xFFFFFF;
}

static inline gboolean
fu_thunderbolt_firmware_valid_pd_pointer (guint32 pointer)
{
	return pointer != 0 && pointer != 0xFFFFFFFF;
}

static gboolean
fu_thunderbolt_firmware_read_location (FuThunderboltFirmware *self,
				       FuThunderboltSection section,
				       guint32 offset,
				       guint8 *buf,
				       guint32 len,
				       GError **error)
{
	const guint8 *srcbuf;
	gsize srcbufsz = 0;
	guint32 location_start = self->sections[section] + offset;
	g_autoptr(GBytes) fw = NULL;

	/* get blob */
	fw = fu_firmware_get_image_default_bytes (FU_FIRMWARE (self), error);
	if (fw == NULL)
		return FALSE;
	srcbuf = g_bytes_get_data (fw, &srcbufsz);

	if (!fu_memcpy_safe (buf, len, 0x0,			/* dst */
			     srcbuf, srcbufsz, location_start,	/* src */
			     len, error)) {
		g_prefix_error (error, "location is outside of the given image: ");
		return FALSE;
	}
	return TRUE;
}

static gboolean
fu_thunderbolt_firmware_read_farb_pointer_impl (FuThunderboltFirmware *self,
						FuThunderboltSection section,
						guint32 offset,
						guint32 *value,
						GError **error)
{
	guint32 tmp = 0;
	if (!fu_thunderbolt_firmware_read_location (self, section, offset,
						    (guint8 *) &tmp, 3, /* 24 bits */
						    error)) {
		g_prefix_error (error, "failed to read farb pointer: ");
		return FALSE;
	}
	*value = GUINT32_FROM_LE (tmp);
	return TRUE;
}

/* returns invalid FARB pointer on error */
static guint32
fu_thunderbolt_firmware_read_farb_pointer (FuThunderboltFirmware *self, GError **error)
{
	guint32 value;
	if (!fu_thunderbolt_firmware_read_farb_pointer_impl (self,
							     _SECTION_DIGITAL,
							     0x0, &value, error))
		return 0;
	if (fu_thunderbolt_firmware_valid_farb_pointer (value))
		return value;

	if (!fu_thunderbolt_firmware_read_farb_pointer_impl (self,
							     _SECTION_DIGITAL,
							     0x1000, &value, error))
		return 0;
	if (!fu_thunderbolt_firmware_valid_farb_pointer (value)) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_INVALID_FILE,
				     "Invalid FW image file format");
		return 0;
	}

	return value;
}

static gboolean
fu_thunderbolt_firmware_read_uint8 (FuThunderboltFirmware *self,
				    FuThunderboltSection section,
				    guint32 offset,
				    guint8 *value,
				    GError **error)
{
	return fu_thunderbolt_firmware_read_location (self, section, offset,
						      value, 1, error);
}

static gboolean
fu_thunderbolt_firmware_read_uint16 (FuThunderboltFirmware *self,
				     FuThunderboltSection section,
				     guint32 offset,
				     guint16 *value,
				     GError **error)
{
	guint16 tmp = 0;
	if (!fu_thunderbolt_firmware_read_location (self, section, offset,
						    (guint8 *) &tmp, sizeof(tmp),
						    error)) {
		g_prefix_error (error, "failed to read uint16: ");
		return FALSE;
	}
	*value = GUINT16_FROM_LE (tmp);
	return TRUE;
}

static gboolean
fu_thunderbolt_firmware_read_uint32 (FuThunderboltFirmware *self,
				     FuThunderboltSection section,
				     guint32 offset,
				     guint32 *value,
				     GError **error)
{
	guint32 tmp = 0;
	if (!fu_thunderbolt_firmware_read_location (self, section, offset,
						    (guint8 *) &tmp, sizeof(tmp),
						    error)) {
		g_prefix_error (error, "failed to read uint32: ");
		return FALSE;
	}
	*value = GUINT32_FROM_LE (tmp);
	return TRUE;
}

/*
 * Size of ucode sections is uint16 value saved at the start of the section,
 * it's in DWORDS (4-bytes) units and it doesn't include itself. We need the
 * offset to the next section, so we translate it to bytes and add 2 for the
 * size field itself.
 *
 * offset parameter must be relative to digital section
 */
static gboolean
fu_thunderbolt_firmware_read_ucode_section_len (FuThunderboltFirmware *self,
					        guint32 offset,
					        guint16 *value,
					        GError **error)
{
	if (!fu_thunderbolt_firmware_read_uint16 (self,
						  _SECTION_DIGITAL,
						  offset,
						  value,
						  error)) {
		g_prefix_error (error, "failed to read ucode section len: ");
		return FALSE;
	}
	*value *= sizeof(guint32);
	*value += sizeof(guint16);
	return TRUE;
}

/* assumes sections[_SECTION_DIGITAL].offset is already set */
static gboolean
fu_thunderbolt_firmware_read_sections (FuThunderboltFirmware *self, GError **error)
{
	guint32 offset;

	if (self->gen >= 3 || self->gen == 0) {
		if (!fu_thunderbolt_firmware_read_uint32 (self,
							  _SECTION_DIGITAL,
							  0x10e,
							  &offset,
							  error))
			return FALSE;
		self->sections[_SECTION_DROM] = offset + self->sections[_SECTION_DIGITAL];

		if (!fu_thunderbolt_firmware_read_uint32 (self,
							  _SECTION_DIGITAL,
							  0x75,
							  &offset,
							  error))
			return FALSE;
		self->sections[_SECTION_ARC_PARAMS] = offset + self->sections[_SECTION_DIGITAL];
	}

	if (self->is_host && self->gen > 2) {
		/*
		 * To find the DRAM section, we have to jump from section to
		 * section in a chain of sections.
		 * available_sections location tells what sections exist at all
		 * (with a flag per section).
		 * ee_ucode_start_addr location tells the offset of the first
		 * section in the list relatively to the digital section start.
		 * After having the offset of the first section, we have a loop
		 * over the section list. If the section exists, we read its
		 * length (2 bytes at section start) and add it to current
		 * offset to find the start of the next section. Otherwise, we
		 * already have the next section offset...
		 */
		const guint8 DRAM_FLAG = 1 << 6;
		guint16 ucode_offset;
		guint8 available_sections = 0;

		if (!fu_thunderbolt_firmware_read_uint8 (self,
							 _SECTION_DIGITAL,
							 0x2,
							 &available_sections,
							 error)) {
			g_prefix_error (error, "failed to read available sections: ");
			return FALSE;
		}
		if (!fu_thunderbolt_firmware_read_uint16 (self,
							  _SECTION_DIGITAL,
							  0x3,
							  &ucode_offset,
							  error)) {
			g_prefix_error (error, "failed to read ucode offset: ");
			return FALSE;
		}
		offset = ucode_offset;
		if ((available_sections & DRAM_FLAG) == 0) {
			g_set_error_literal (error,
					     FWUPD_ERROR, FWUPD_ERROR_INVALID_FILE,
					     "Can't find needed FW sections in the FW image file");
			return FALSE;
		}

		for (guint8 i = 1; i < DRAM_FLAG; i <<= 1) {
			if (available_sections & i) {
				if (!fu_thunderbolt_firmware_read_ucode_section_len (self,
										     offset,
										     &ucode_offset,
										     error))
					return FALSE;
				offset += ucode_offset;
			}
		}
		self->sections[_SECTION_DRAM_UCODE] = offset + self->sections[_SECTION_DIGITAL];
	}

	return TRUE;
}

static gboolean
fu_thunderbolt_firmware_missing_needed_drom (FuThunderboltFirmware *self)
{
	if (self->sections[_SECTION_DROM] != 0)
		return FALSE;
	if (self->is_host && self->gen < 3)
		return FALSE;
	return TRUE;
}

static gboolean
fu_thunderbolt_firmware_parse (FuFirmware *firmware,
			       GBytes *fw,
			       guint64 addr_start,
			       guint64 addr_end,
			       FwupdInstallFlags flags,
			       GError **error)
{
	FuThunderboltFirmware *self = FU_THUNDERBOLT_FIRMWARE (firmware);
	guint8 tmp = 0;
	static const FuThunderboltHwInfo hw_info_arr[] = {
		{ 0x156D, 2, _FAMILY_FR, 2 }, /* FR 4C */
		{ 0x156B, 2, _FAMILY_FR, 1 }, /* FR 2C */
		{ 0x157E, 2, _FAMILY_WR, 1 }, /* WR */
		{ 0x1578, 3, _FAMILY_AR, 2 }, /* AR 4C */
		{ 0x1576, 3, _FAMILY_AR, 1 }, /* AR 2C */
		{ 0x15C0, 3, _FAMILY_AR, 1 }, /* AR LP */
		{ 0x15D3, 3, _FAMILY_AR_C, 2 }, /* AR-C 4C */
		{ 0x15DA, 3, _FAMILY_AR_C, 1 }, /* AR-C 2C */
		{ 0x15E7, 3, _FAMILY_TR, 1 }, /* TR 2C */
		{ 0x15EA, 3, _FAMILY_TR, 2 }, /* TR 4C */
		{ 0x15EF, 3, _FAMILY_TR, 2 }, /* TR 4C device */
		{ 0x15EE, 3, _FAMILY_BB, 0 }, /* BB device */
		{ 0 }
	};

	g_autoptr(FuFirmwareImage) img = fu_firmware_image_new (fw);

	/* add this straight away so we can read it without a self */
	fu_firmware_add_image (firmware, img);

	/* is native */
	if (!fu_thunderbolt_firmware_read_uint8 (self,
						 _SECTION_DIGITAL,
						 FU_TBT_OFFSET_NATIVE,
						 &tmp, error)) {
		g_prefix_error (error, "failed to read native: ");
		return FALSE;
	}
	self->is_native = tmp & 0x20;

	self->sections[_SECTION_DIGITAL] = fu_thunderbolt_firmware_read_farb_pointer (self, error);
	if (self->sections[_SECTION_DIGITAL] == 0)
		return FALSE;

	/* we're only reading the first chunk */
	if (g_bytes_get_size (fw) == 0x80)
		return TRUE;

	/* host or device */
	if (!fu_thunderbolt_firmware_read_uint8 (self,
						 _SECTION_DIGITAL,
						 0x10, &tmp, error)) {
		g_prefix_error (error, "failed to read is-host: ");
		return FALSE;
	}
	self->is_host = tmp & (1 << 1);

	/* device ID */
	if (!fu_thunderbolt_firmware_read_uint16 (self,
						  _SECTION_DIGITAL,
						  0x5,
						  &self->device_id,
						  error)) {
		g_prefix_error (error, "failed to read device-id: ");
		return FALSE;
	}

	/* this is best-effort */
	for (guint i = 0; hw_info_arr[i].id != 0; i++) {
		if (hw_info_arr[i].id == self->device_id) {
			self->family = hw_info_arr[i].family;
			self->gen = hw_info_arr[i].gen;
			self->ports = hw_info_arr[i].ports;
			break;
		}
	}
	if (self->ports == 0 && self->is_host) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_NOT_SUPPORTED,
			     "Unknown controller");
		return FALSE;
	}

	/* read sections from file */
	if (!fu_thunderbolt_firmware_read_sections (self, error))
		return FALSE;
	if (fu_thunderbolt_firmware_missing_needed_drom (self)) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_READ,
				     "Can't find required FW sections");
		return FALSE;
	}

	/* vendor:model */
	if (self->sections[_SECTION_DROM] != 0) {
		if (!fu_thunderbolt_firmware_read_uint16 (self,
							  _SECTION_DROM,
							  0x10,
							  &self->vendor_id,
							  error)) {
			g_prefix_error (error, "failed to read vendor-id: ");
			return FALSE;
		}
		if (!fu_thunderbolt_firmware_read_uint16 (self,
							  _SECTION_DROM,
							  0x12,
							  &self->model_id,
							  error)) {
			g_prefix_error (error, "failed to read model-id: ");
			return FALSE;
		}
	}

	/* has PD */
	if (self->sections[_SECTION_ARC_PARAMS] != 0) {
		guint32 pd_pointer = 0x0;
		if (!fu_thunderbolt_firmware_read_uint32 (self,
							  _SECTION_ARC_PARAMS,
							  0x10C,
							  &pd_pointer,
							  error)) {
			g_prefix_error (error, "failed to read pd-pointer: ");
			return FALSE;
		}
		self->has_pd = fu_thunderbolt_firmware_valid_pd_pointer (pd_pointer);
	}

	if (self->is_host) {
		switch (self->family) {
		case _FAMILY_AR:
		case _FAMILY_AR_C:
		case _FAMILY_TR:
			/* This is used for comparison between old and new image, not a raw number */
			if (!fu_thunderbolt_firmware_read_uint8 (self,
								 _SECTION_DIGITAL,
								 0x45,
								 &tmp,
								 error)) {
				g_prefix_error (error, "failed to read flash size: ");
				return FALSE;
			}
			self->flash_size = tmp & 0x07;
			break;
		default:
			break;
		}
	}

	/* success */
	return TRUE;
}

static void
fu_thunderbolt_firmware_init (FuThunderboltFirmware *self)
{
}

static void
fu_thunderbolt_firmware_class_init (FuThunderboltFirmwareClass *klass)
{
	FuFirmwareClass *klass_firmware = FU_FIRMWARE_CLASS (klass);
	klass_firmware->parse = fu_thunderbolt_firmware_parse;
	klass_firmware->to_string = fu_thunderbolt_firmware_to_string;
}

FuThunderboltFirmware *
fu_thunderbolt_firmware_new (void)
{
	return g_object_new (FU_TYPE_THUNDERBOLT_FIRMWARE, NULL);
}