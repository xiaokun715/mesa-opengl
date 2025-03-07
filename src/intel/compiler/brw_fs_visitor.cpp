/*
 * Copyright © 2010 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

/** @file brw_fs_visitor.cpp
 *
 * This file supports generating the FS LIR from the GLSL IR.  The LIR
 * makes it easier to do backend-specific optimizations than doing so
 * in the GLSL IR or in the native code.
 */
#include "brw_eu.h"
#include "brw_fs.h"
#include "brw_fs_builder.h"
#include "brw_nir.h"
#include "compiler/glsl_types.h"
#include "dev/intel_device_info.h"

using namespace brw;

/* Input data is organized with first the per-primitive values, followed
 * by per-vertex values.  The per-vertex will have interpolation information
 * associated, so use 4 components for each value.
 */

/* The register location here is relative to the start of the URB
 * data.  It will get adjusted to be a real location before
 * generate_code() time.
 */
brw_reg
fs_visitor::interp_reg(const fs_builder &bld, unsigned location,
                       unsigned channel, unsigned comp)
{
   assert(stage == MESA_SHADER_FRAGMENT);
   assert(BITFIELD64_BIT(location) & ~nir->info.per_primitive_inputs);

   const struct brw_wm_prog_data *prog_data = brw_wm_prog_data(this->prog_data);

   assert(prog_data->urb_setup[location] >= 0);
   unsigned nr = prog_data->urb_setup[location];
   channel += prog_data->urb_setup_channel[location];

   /* Adjust so we start counting from the first per_vertex input. */
   assert(nr >= prog_data->num_per_primitive_inputs);
   nr -= prog_data->num_per_primitive_inputs;

   const unsigned per_vertex_start = prog_data->num_per_primitive_inputs;
   const unsigned regnr = per_vertex_start + (nr * 4) + channel;

   if (max_polygons > 1) {
      /* In multipolygon dispatch each plane parameter is a
       * dispatch_width-wide SIMD vector (see comment in
       * assign_urb_setup()), so we need to use offset() instead of
       * component() to select the specified parameter.
       */
      const brw_reg tmp = bld.vgrf(BRW_TYPE_UD);
      bld.MOV(tmp, offset(brw_attr_reg(regnr, BRW_TYPE_UD),
                          dispatch_width, comp));
      return retype(tmp, BRW_TYPE_F);
   } else {
      return component(brw_attr_reg(regnr, BRW_TYPE_F), comp);
   }
}

/* The register location here is relative to the start of the URB
 * data.  It will get adjusted to be a real location before
 * generate_code() time.
 */
brw_reg
fs_visitor::per_primitive_reg(const fs_builder &bld, int location, unsigned comp)
{
   assert(stage == MESA_SHADER_FRAGMENT);
   assert(BITFIELD64_BIT(location) & nir->info.per_primitive_inputs);

   const struct brw_wm_prog_data *prog_data = brw_wm_prog_data(this->prog_data);

   comp += prog_data->urb_setup_channel[location];

   assert(prog_data->urb_setup[location] >= 0);

   const unsigned regnr = prog_data->urb_setup[location] + comp / 4;

   assert(regnr < prog_data->num_per_primitive_inputs);

   if (max_polygons > 1) {
      /* In multipolygon dispatch each primitive constant is a
       * dispatch_width-wide SIMD vector (see comment in
       * assign_urb_setup()), so we need to use offset() instead of
       * component() to select the specified parameter.
       */
      const brw_reg tmp = bld.vgrf(BRW_TYPE_UD);
      bld.MOV(tmp, offset(brw_attr_reg(regnr, BRW_TYPE_UD),
                          dispatch_width, comp % 4));
      return retype(tmp, BRW_TYPE_F);
   } else {
      return component(brw_attr_reg(regnr, BRW_TYPE_F), comp % 4);
   }
}

/** Emits the interpolation for the varying inputs. */
void
fs_visitor::emit_interpolation_setup()
{
   const fs_builder bld = fs_builder(this).at_end();
   fs_builder abld = bld.annotate("compute pixel centers");

   this->pixel_x = bld.vgrf(BRW_TYPE_F);
   this->pixel_y = bld.vgrf(BRW_TYPE_F);

   const struct brw_wm_prog_key *wm_key = (brw_wm_prog_key*) this->key;
   struct brw_wm_prog_data *wm_prog_data = brw_wm_prog_data(prog_data);

   brw_reg int_sample_offset_x, int_sample_offset_y; /* Used on Gen12HP+ */
   brw_reg int_sample_offset_xy; /* Used on Gen8+ */
   brw_reg half_int_sample_offset_x, half_int_sample_offset_y;
   if (wm_prog_data->coarse_pixel_dispatch != BRW_ALWAYS) {
      /* The thread payload only delivers subspan locations (ss0, ss1,
       * ss2, ...). Since subspans covers 2x2 pixels blocks, we need to
       * generate 4 pixel coordinates out of each subspan location. We do this
       * by replicating a subspan coordinate 4 times and adding an offset of 1
       * in each direction from the initial top left (tl) location to generate
       * top right (tr = +1 in x), bottom left (bl = +1 in y) and bottom right
       * (br = +1 in x, +1 in y).
       *
       * The locations we build look like this in SIMD8 :
       *
       *    ss0.tl ss0.tr ss0.bl ss0.br ss1.tl ss1.tr ss1.bl ss1.br
       *
       * The value 0x11001010 is a vector of 8 half byte vector. It adds
       * following to generate the 4 pixels coordinates out of the subspan0:
       *
       *  0x
       *    1 : ss0.y + 1 -> ss0.br.y
       *    1 : ss0.y + 1 -> ss0.bl.y
       *    0 : ss0.y + 0 -> ss0.tr.y
       *    0 : ss0.y + 0 -> ss0.tl.y
       *    1 : ss0.x + 1 -> ss0.br.x
       *    0 : ss0.x + 0 -> ss0.bl.x
       *    1 : ss0.x + 1 -> ss0.tr.x
       *    0 : ss0.x + 0 -> ss0.tl.x
       *
       * By doing a SIMD16 add in a SIMD8 shader, we can generate the 8 pixels
       * coordinates out of 2 subspans coordinates in a single ADD instruction
       * (twice the operation above).
       */
      int_sample_offset_xy = brw_reg(brw_imm_v(0x11001010));
      half_int_sample_offset_x = brw_reg(brw_imm_uw(0));
      half_int_sample_offset_y = brw_reg(brw_imm_uw(0));
      /* On Gfx12.5, because of regioning restrictions, the interpolation code
       * is slightly different and works off X & Y only inputs. The ordering
       * of the half bytes here is a bit odd, with each subspan replicated
       * twice and every other element is discarded :
       *
       *             ss0.tl ss0.tl ss0.tr ss0.tr ss0.bl ss0.bl ss0.br ss0.br
       *  X offset:    0      0      1      0      0      0      1      0
       *  Y offset:    0      0      0      0      1      0      1      0
       */
      int_sample_offset_x = brw_reg(brw_imm_v(0x01000100));
      int_sample_offset_y = brw_reg(brw_imm_v(0x01010000));
   }

   brw_reg int_coarse_offset_x, int_coarse_offset_y; /* Used on Gen12HP+ */
   brw_reg int_coarse_offset_xy; /* Used on Gen8+ */
   brw_reg half_int_coarse_offset_x, half_int_coarse_offset_y;
   if (wm_prog_data->coarse_pixel_dispatch != BRW_NEVER) {
      /* In coarse pixel dispatch we have to do the same ADD instruction that
       * we do in normal per pixel dispatch, except this time we're not adding
       * 1 in each direction, but instead the coarse pixel size.
       *
       * The coarse pixel size is delivered as 2 u8 in r1.0
       */
      struct brw_reg r1_0 = retype(brw_vec1_reg(BRW_GENERAL_REGISTER_FILE, 1, 0), BRW_TYPE_UB);

      const fs_builder dbld =
         abld.exec_all().group(MIN2(16, dispatch_width) * 2, 0);

      if (devinfo->verx10 >= 125) {
         /* To build the array of half bytes we do and AND operation with the
          * right mask in X.
          */
         int_coarse_offset_x = dbld.vgrf(BRW_TYPE_UW);
         dbld.AND(int_coarse_offset_x, byte_offset(r1_0, 0), brw_imm_v(0x0f000f00));

         /* And the right mask in Y. */
         int_coarse_offset_y = dbld.vgrf(BRW_TYPE_UW);
         dbld.AND(int_coarse_offset_y, byte_offset(r1_0, 1), brw_imm_v(0x0f0f0000));
      } else {
         /* To build the array of half bytes we do and AND operation with the
          * right mask in X.
          */
         int_coarse_offset_x = dbld.vgrf(BRW_TYPE_UW);
         dbld.AND(int_coarse_offset_x, byte_offset(r1_0, 0), brw_imm_v(0x0000f0f0));

         /* And the right mask in Y. */
         int_coarse_offset_y = dbld.vgrf(BRW_TYPE_UW);
         dbld.AND(int_coarse_offset_y, byte_offset(r1_0, 1), brw_imm_v(0xff000000));

         /* Finally OR the 2 registers. */
         int_coarse_offset_xy = dbld.vgrf(BRW_TYPE_UW);
         dbld.OR(int_coarse_offset_xy, int_coarse_offset_x, int_coarse_offset_y);
      }

      /* Also compute the half coarse size used to center coarses. */
      half_int_coarse_offset_x = bld.vgrf(BRW_TYPE_UW);
      half_int_coarse_offset_y = bld.vgrf(BRW_TYPE_UW);

      bld.SHR(half_int_coarse_offset_x, suboffset(r1_0, 0), brw_imm_ud(1));
      bld.SHR(half_int_coarse_offset_y, suboffset(r1_0, 1), brw_imm_ud(1));
   }

   brw_reg int_pixel_offset_x, int_pixel_offset_y; /* Used on Gen12HP+ */
   brw_reg int_pixel_offset_xy; /* Used on Gen8+ */
   brw_reg half_int_pixel_offset_x, half_int_pixel_offset_y;
   switch (wm_prog_data->coarse_pixel_dispatch) {
   case BRW_NEVER:
      int_pixel_offset_x = int_sample_offset_x;
      int_pixel_offset_y = int_sample_offset_y;
      int_pixel_offset_xy = int_sample_offset_xy;
      half_int_pixel_offset_x = half_int_sample_offset_x;
      half_int_pixel_offset_y = half_int_sample_offset_y;
      break;

   case BRW_SOMETIMES: {
      const fs_builder dbld =
         abld.exec_all().group(MIN2(16, dispatch_width) * 2, 0);

      check_dynamic_msaa_flag(dbld, wm_prog_data,
                              INTEL_MSAA_FLAG_COARSE_RT_WRITES);

      int_pixel_offset_x = dbld.vgrf(BRW_TYPE_UW);
      set_predicate(BRW_PREDICATE_NORMAL,
                    dbld.SEL(int_pixel_offset_x,
                             int_coarse_offset_x,
                             int_sample_offset_x));

      int_pixel_offset_y = dbld.vgrf(BRW_TYPE_UW);
      set_predicate(BRW_PREDICATE_NORMAL,
                    dbld.SEL(int_pixel_offset_y,
                             int_coarse_offset_y,
                             int_sample_offset_y));

      int_pixel_offset_xy = dbld.vgrf(BRW_TYPE_UW);
      set_predicate(BRW_PREDICATE_NORMAL,
                    dbld.SEL(int_pixel_offset_xy,
                             int_coarse_offset_xy,
                             int_sample_offset_xy));

      half_int_pixel_offset_x = bld.vgrf(BRW_TYPE_UW);
      set_predicate(BRW_PREDICATE_NORMAL,
                    bld.SEL(half_int_pixel_offset_x,
                            half_int_coarse_offset_x,
                            half_int_sample_offset_x));

      half_int_pixel_offset_y = bld.vgrf(BRW_TYPE_UW);
      set_predicate(BRW_PREDICATE_NORMAL,
                    bld.SEL(half_int_pixel_offset_y,
                            half_int_coarse_offset_y,
                            half_int_sample_offset_y));
      break;
   }

   case BRW_ALWAYS:
      int_pixel_offset_x = int_coarse_offset_x;
      int_pixel_offset_y = int_coarse_offset_y;
      int_pixel_offset_xy = int_coarse_offset_xy;
      half_int_pixel_offset_x = half_int_coarse_offset_x;
      half_int_pixel_offset_y = half_int_coarse_offset_y;
      break;
   }

   for (unsigned i = 0; i < DIV_ROUND_UP(dispatch_width, 16); i++) {
      const fs_builder hbld = abld.group(MIN2(16, dispatch_width), i);
      /* According to the "PS Thread Payload for Normal Dispatch"
       * pages on the BSpec, subspan X/Y coordinates are stored in
       * R1.2-R1.5/R2.2-R2.5 on gfx6+, and on R0.10-R0.13/R1.10-R1.13
       * on gfx20+.  gi_reg is the 32B section of the GRF that
       * contains the subspan coordinates.
       */
      const struct brw_reg gi_reg = devinfo->ver >= 20 ? xe2_vec1_grf(i, 8) :
                                    brw_vec1_grf(i + 1, 0);
      const struct brw_reg gi_uw = retype(gi_reg, BRW_TYPE_UW);

      if (devinfo->verx10 >= 125) {
         const fs_builder dbld =
            abld.exec_all().group(hbld.dispatch_width() * 2, 0);
         const brw_reg int_pixel_x = dbld.vgrf(BRW_TYPE_UW);
         const brw_reg int_pixel_y = dbld.vgrf(BRW_TYPE_UW);

         dbld.ADD(int_pixel_x,
                  brw_reg(stride(suboffset(gi_uw, 4), 2, 8, 0)),
                  int_pixel_offset_x);
         dbld.ADD(int_pixel_y,
                  brw_reg(stride(suboffset(gi_uw, 5), 2, 8, 0)),
                  int_pixel_offset_y);

         if (wm_prog_data->coarse_pixel_dispatch != BRW_NEVER) {
            fs_inst *addx = dbld.ADD(int_pixel_x, int_pixel_x,
                                     horiz_stride(half_int_pixel_offset_x, 0));
            fs_inst *addy = dbld.ADD(int_pixel_y, int_pixel_y,
                                     horiz_stride(half_int_pixel_offset_y, 0));
            if (wm_prog_data->coarse_pixel_dispatch != BRW_ALWAYS) {
               addx->predicate = BRW_PREDICATE_NORMAL;
               addy->predicate = BRW_PREDICATE_NORMAL;
            }
         }

         hbld.MOV(offset(pixel_x, hbld, i), horiz_stride(int_pixel_x, 2));
         hbld.MOV(offset(pixel_y, hbld, i), horiz_stride(int_pixel_y, 2));

      } else {
         /* The "Register Region Restrictions" page says for BDW (and newer,
          * presumably):
          *
          *     "When destination spans two registers, the source may be one or
          *      two registers. The destination elements must be evenly split
          *      between the two registers."
          *
          * Thus we can do a single add(16) in SIMD8 or an add(32) in SIMD16
          * to compute our pixel centers.
          */
         const fs_builder dbld =
            abld.exec_all().group(hbld.dispatch_width() * 2, 0);
         brw_reg int_pixel_xy = dbld.vgrf(BRW_TYPE_UW);

         dbld.ADD(int_pixel_xy,
                  brw_reg(stride(suboffset(gi_uw, 4), 1, 4, 0)),
                  int_pixel_offset_xy);

         hbld.emit(FS_OPCODE_PIXEL_X, offset(pixel_x, hbld, i), int_pixel_xy,
                                      horiz_stride(half_int_pixel_offset_x, 0));
         hbld.emit(FS_OPCODE_PIXEL_Y, offset(pixel_y, hbld, i), int_pixel_xy,
                                      horiz_stride(half_int_pixel_offset_y, 0));
      }
   }

   abld = bld.annotate("compute pos.z");
   brw_reg coarse_z;
   if (wm_prog_data->coarse_pixel_dispatch != BRW_NEVER &&
       wm_prog_data->uses_depth_w_coefficients) {
      /* In coarse pixel mode, the HW doesn't interpolate Z coordinate
       * properly. In the same way we have to add the coarse pixel size to
       * pixels locations, here we recompute the Z value with 2 coefficients
       * in X & Y axis.
       */
      brw_reg coef_payload = brw_vec8_grf(fs_payload().depth_w_coef_reg, 0);
      const brw_reg x_start = brw_vec1_grf(coef_payload.nr, 2);
      const brw_reg y_start = brw_vec1_grf(coef_payload.nr, 6);
      const brw_reg z_cx    = brw_vec1_grf(coef_payload.nr, 1);
      const brw_reg z_cy    = brw_vec1_grf(coef_payload.nr, 0);
      const brw_reg z_c0    = brw_vec1_grf(coef_payload.nr, 3);

      const brw_reg float_pixel_x = abld.vgrf(BRW_TYPE_F);
      const brw_reg float_pixel_y = abld.vgrf(BRW_TYPE_F);

      abld.ADD(float_pixel_x, this->pixel_x, negate(x_start));
      abld.ADD(float_pixel_y, this->pixel_y, negate(y_start));

      /* r1.0 - 0:7 ActualCoarsePixelShadingSize.X */
      const brw_reg u8_cps_width = brw_reg(retype(brw_vec1_grf(1, 0), BRW_TYPE_UB));
      /* r1.0 - 15:8 ActualCoarsePixelShadingSize.Y */
      const brw_reg u8_cps_height = byte_offset(u8_cps_width, 1);
      const brw_reg u32_cps_width = abld.vgrf(BRW_TYPE_UD);
      const brw_reg u32_cps_height = abld.vgrf(BRW_TYPE_UD);
      abld.MOV(u32_cps_width, u8_cps_width);
      abld.MOV(u32_cps_height, u8_cps_height);

      const brw_reg f_cps_width = abld.vgrf(BRW_TYPE_F);
      const brw_reg f_cps_height = abld.vgrf(BRW_TYPE_F);
      abld.MOV(f_cps_width, u32_cps_width);
      abld.MOV(f_cps_height, u32_cps_height);

      /* Center in the middle of the coarse pixel. */
      abld.MAD(float_pixel_x, float_pixel_x, brw_imm_f(0.5f), f_cps_width);
      abld.MAD(float_pixel_y, float_pixel_y, brw_imm_f(0.5f), f_cps_height);

      coarse_z = abld.vgrf(BRW_TYPE_F);
      abld.MAD(coarse_z, z_c0, z_cx, float_pixel_x);
      abld.MAD(coarse_z, coarse_z, z_cy, float_pixel_y);
   }

   if (wm_prog_data->uses_src_depth)
      this->pixel_z = fetch_payload_reg(bld, fs_payload().source_depth_reg);

   if (wm_prog_data->uses_depth_w_coefficients ||
       wm_prog_data->uses_src_depth) {
      brw_reg sample_z = this->pixel_z;

      switch (wm_prog_data->coarse_pixel_dispatch) {
      case BRW_NEVER:
         break;

      case BRW_SOMETIMES:
         assert(wm_prog_data->uses_src_depth);
         assert(wm_prog_data->uses_depth_w_coefficients);
         this->pixel_z = abld.vgrf(BRW_TYPE_F);

         /* We re-use the check_dynamic_msaa_flag() call from above */
         set_predicate(BRW_PREDICATE_NORMAL,
                       abld.SEL(this->pixel_z, coarse_z, sample_z));
         break;

      case BRW_ALWAYS:
         assert(!wm_prog_data->uses_src_depth);
         assert(wm_prog_data->uses_depth_w_coefficients);
         this->pixel_z = coarse_z;
         break;
      }
   }

   if (wm_prog_data->uses_src_w) {
      abld = bld.annotate("compute pos.w");
      this->pixel_w = fetch_payload_reg(abld, fs_payload().source_w_reg);
      this->wpos_w = bld.vgrf(BRW_TYPE_F);
      abld.emit(SHADER_OPCODE_RCP, this->wpos_w, this->pixel_w);
   }

   if (wm_key->persample_interp == BRW_SOMETIMES) {
      assert(!devinfo->needs_unlit_centroid_workaround);

      const fs_builder ubld = bld.exec_all().group(16, 0);
      bool loaded_flag = false;

      for (int i = 0; i < BRW_BARYCENTRIC_MODE_COUNT; ++i) {
         if (!(wm_prog_data->barycentric_interp_modes & BITFIELD_BIT(i)))
            continue;

         /* The sample mode will always be the top bit set in the perspective
          * or non-perspective section.  In the case where no SAMPLE mode was
          * requested, wm_prog_data_barycentric_modes() will swap out the top
          * mode for SAMPLE so this works regardless of whether SAMPLE was
          * requested or not.
          */
         int sample_mode;
         if (BITFIELD_BIT(i) & BRW_BARYCENTRIC_NONPERSPECTIVE_BITS) {
            sample_mode = util_last_bit(wm_prog_data->barycentric_interp_modes &
                                        BRW_BARYCENTRIC_NONPERSPECTIVE_BITS) - 1;
         } else {
            sample_mode = util_last_bit(wm_prog_data->barycentric_interp_modes &
                                        BRW_BARYCENTRIC_PERSPECTIVE_BITS) - 1;
         }
         assert(wm_prog_data->barycentric_interp_modes &
                BITFIELD_BIT(sample_mode));

         if (i == sample_mode)
            continue;

         uint8_t *barys = fs_payload().barycentric_coord_reg[i];

         uint8_t *sample_barys = fs_payload().barycentric_coord_reg[sample_mode];
         assert(barys[0] && sample_barys[0]);

         if (!loaded_flag) {
            check_dynamic_msaa_flag(ubld, wm_prog_data,
                                    INTEL_MSAA_FLAG_PERSAMPLE_INTERP);
         }

         for (unsigned j = 0; j < dispatch_width / 8; j++) {
            set_predicate(
               BRW_PREDICATE_NORMAL,
               ubld.MOV(brw_vec8_grf(barys[j / 2] + (j % 2) * 2, 0),
                        brw_vec8_grf(sample_barys[j / 2] + (j % 2) * 2, 0)));
         }
      }
   }

   for (int i = 0; i < BRW_BARYCENTRIC_MODE_COUNT; ++i) {
      this->delta_xy[i] = fetch_barycentric_reg(
         bld, fs_payload().barycentric_coord_reg[i]);
   }

   uint32_t centroid_modes = wm_prog_data->barycentric_interp_modes &
      (1 << BRW_BARYCENTRIC_PERSPECTIVE_CENTROID |
       1 << BRW_BARYCENTRIC_NONPERSPECTIVE_CENTROID);

   if (devinfo->needs_unlit_centroid_workaround && centroid_modes) {
      /* Get the pixel/sample mask into f0 so that we know which
       * pixels are lit.  Then, for each channel that is unlit,
       * replace the centroid data with non-centroid data.
       */
      for (unsigned i = 0; i < DIV_ROUND_UP(dispatch_width, 16); i++) {
         bld.exec_all().group(1, 0)
            .MOV(retype(brw_flag_reg(0, i), BRW_TYPE_UW),
                 retype(brw_vec1_grf(1 + i, 7), BRW_TYPE_UW));
      }

      for (int i = 0; i < BRW_BARYCENTRIC_MODE_COUNT; ++i) {
         if (!(centroid_modes & (1 << i)))
            continue;

         const brw_reg centroid_delta_xy = delta_xy[i];
         const brw_reg &pixel_delta_xy = delta_xy[i - 1];

         delta_xy[i] = bld.vgrf(BRW_TYPE_F, 2);

         for (unsigned c = 0; c < 2; c++) {
            for (unsigned q = 0; q < dispatch_width / 8; q++) {
               set_predicate(BRW_PREDICATE_NORMAL,
                  bld.quarter(q).SEL(
                     quarter(offset(delta_xy[i], bld, c), q),
                     quarter(offset(centroid_delta_xy, bld, c), q),
                     quarter(offset(pixel_delta_xy, bld, c), q)));
            }
         }
      }
   }
}

fs_inst *
fs_visitor::emit_single_fb_write(const fs_builder &bld,
                                 brw_reg color0, brw_reg color1,
                                 brw_reg src0_alpha, unsigned components)
{
   assert(stage == MESA_SHADER_FRAGMENT);
   struct brw_wm_prog_data *prog_data = brw_wm_prog_data(this->prog_data);

   /* Hand over gl_FragDepth or the payload depth. */
   const brw_reg dst_depth = fetch_payload_reg(bld, fs_payload().dest_depth_reg);
   brw_reg src_depth, src_stencil;

   if (nir->info.outputs_written & BITFIELD64_BIT(FRAG_RESULT_DEPTH))
      src_depth = frag_depth;

   if (nir->info.outputs_written & BITFIELD64_BIT(FRAG_RESULT_STENCIL))
      src_stencil = frag_stencil;

   const brw_reg sources[] = {
      color0, color1, src0_alpha, src_depth, dst_depth, src_stencil,
      (prog_data->uses_omask ? sample_mask : brw_reg()),
      brw_imm_ud(components)
   };
   assert(ARRAY_SIZE(sources) - 1 == FB_WRITE_LOGICAL_SRC_COMPONENTS);
   fs_inst *write = bld.emit(FS_OPCODE_FB_WRITE_LOGICAL, brw_reg(),
                             sources, ARRAY_SIZE(sources));

   if (prog_data->uses_kill) {
      write->predicate = BRW_PREDICATE_NORMAL;
      write->flag_subreg = sample_mask_flag_subreg(*this);
   }

   return write;
}

void
fs_visitor::do_emit_fb_writes(int nr_color_regions, bool replicate_alpha)
{
   const fs_builder bld = fs_builder(this).at_end();
   fs_inst *inst = NULL;

   for (int target = 0; target < nr_color_regions; target++) {
      /* Skip over outputs that weren't written. */
      if (this->outputs[target].file == BAD_FILE)
         continue;

      const fs_builder abld = bld.annotate(
         ralloc_asprintf(this->mem_ctx, "FB write target %d", target));

      brw_reg src0_alpha;
      if (replicate_alpha && target != 0)
         src0_alpha = offset(outputs[0], bld, 3);

      inst = emit_single_fb_write(abld, this->outputs[target],
                                  this->dual_src_output, src0_alpha, 4);
      inst->target = target;
   }

   if (inst == NULL) {
      /* Even if there's no color buffers enabled, we still need to send
       * alpha out the pipeline to our null renderbuffer to support
       * alpha-testing, alpha-to-coverage, and so on.
       */
      /* FINISHME: Factor out this frequently recurring pattern into a
       * helper function.
       */
      const brw_reg srcs[] = { reg_undef, reg_undef,
                              reg_undef, offset(this->outputs[0], bld, 3) };
      const brw_reg tmp = bld.vgrf(BRW_TYPE_UD, 4);
      bld.LOAD_PAYLOAD(tmp, srcs, 4, 0);

      inst = emit_single_fb_write(bld, tmp, reg_undef, reg_undef, 4);
      inst->target = 0;
   }

   inst->last_rt = true;
   inst->eot = true;
}

void
fs_visitor::emit_fb_writes()
{
   assert(stage == MESA_SHADER_FRAGMENT);
   struct brw_wm_prog_data *prog_data = brw_wm_prog_data(this->prog_data);
   brw_wm_prog_key *key = (brw_wm_prog_key*) this->key;

   if (nir->info.outputs_written & BITFIELD64_BIT(FRAG_RESULT_STENCIL)) {
      /* From the 'Render Target Write message' section of the docs:
       * "Output Stencil is not supported with SIMD16 Render Target Write
       * Messages."
       */
      if (devinfo->ver >= 20)
         limit_dispatch_width(16, "gl_FragStencilRefARB unsupported "
                              "in SIMD32+ mode.\n");
      else
         limit_dispatch_width(8, "gl_FragStencilRefARB unsupported "
                              "in SIMD16+ mode.\n");
   }

   /* ANV doesn't know about sample mask output during the wm key creation
    * so we compute if we need replicate alpha and emit alpha to coverage
    * workaround here.
    */
   const bool replicate_alpha = key->alpha_test_replicate_alpha ||
      (key->nr_color_regions > 1 && key->alpha_to_coverage &&
       sample_mask.file == BAD_FILE);

   prog_data->dual_src_blend = (this->dual_src_output.file != BAD_FILE &&
                                this->outputs[0].file != BAD_FILE);
   assert(!prog_data->dual_src_blend || key->nr_color_regions == 1);

   /* Following condition implements Wa_14017468336:
    *
    * "If dual source blend is enabled do not enable SIMD32 dispatch" and
    * "For a thread dispatched as SIMD32, must not issue SIMD8 message with Last
    *  Render Target Select set."
    */
   if (devinfo->ver >= 11 && devinfo->ver <= 12 &&
       prog_data->dual_src_blend) {
      /* The dual-source RT write messages fail to release the thread
       * dependency on ICL and TGL with SIMD32 dispatch, leading to hangs.
       *
       * XXX - Emit an extra single-source NULL RT-write marked LastRT in
       *       order to release the thread dependency without disabling
       *       SIMD32.
       *
       * The dual-source RT write messages may lead to hangs with SIMD16
       * dispatch on ICL due some unknown reasons, see
       * https://gitlab.freedesktop.org/mesa/mesa/-/issues/2183
       */
      if (devinfo->ver >= 20)
         limit_dispatch_width(16, "Dual source blending unsupported "
                              "in SIMD32 mode.\n");
      else
         limit_dispatch_width(8, "Dual source blending unsupported "
                              "in SIMD16 and SIMD32 modes.\n");
   }

   do_emit_fb_writes(key->nr_color_regions, replicate_alpha);
}

void
fs_visitor::emit_urb_writes(const brw_reg &gs_vertex_count)
{
   int slot, urb_offset, length;
   int starting_urb_offset = 0;
   const struct brw_vue_prog_data *vue_prog_data =
      brw_vue_prog_data(this->prog_data);
   const GLbitfield64 psiz_mask =
      VARYING_BIT_LAYER | VARYING_BIT_VIEWPORT | VARYING_BIT_PSIZ | VARYING_BIT_PRIMITIVE_SHADING_RATE;
   const struct intel_vue_map *vue_map = &vue_prog_data->vue_map;
   bool flush;
   brw_reg sources[8];
   brw_reg urb_handle;

   switch (stage) {
   case MESA_SHADER_VERTEX:
      urb_handle = vs_payload().urb_handles;
      break;
   case MESA_SHADER_TESS_EVAL:
      urb_handle = tes_payload().urb_output;
      break;
   case MESA_SHADER_GEOMETRY:
      urb_handle = gs_payload().urb_handles;
      break;
   default:
      unreachable("invalid stage");
   }

   const fs_builder bld = fs_builder(this).at_end();

   brw_reg per_slot_offsets;

   if (stage == MESA_SHADER_GEOMETRY) {
      const struct brw_gs_prog_data *gs_prog_data =
         brw_gs_prog_data(this->prog_data);

      /* We need to increment the Global Offset to skip over the control data
       * header and the extra "Vertex Count" field (1 HWord) at the beginning
       * of the VUE.  We're counting in OWords, so the units are doubled.
       */
      starting_urb_offset = 2 * gs_prog_data->control_data_header_size_hwords;
      if (gs_prog_data->static_vertex_count == -1)
         starting_urb_offset += 2;

      /* The URB offset is in 128-bit units, so we need to multiply by 2 */
      const int output_vertex_size_owords =
         gs_prog_data->output_vertex_size_hwords * 2;

      /* On Xe2+ platform, LSC can operate on the Dword data element with byte
       * offset granularity, so convert per slot offset in bytes since it's in
       * Owords (16-bytes) unit else keep per slot offset in oword unit for
       * previous platforms.
       */
      const int output_vertex_size = devinfo->ver >= 20 ?
                                     output_vertex_size_owords * 16 :
                                     output_vertex_size_owords;
      if (gs_vertex_count.file == IMM) {
         per_slot_offsets = brw_imm_ud(output_vertex_size *
                                       gs_vertex_count.ud);
      } else {
         per_slot_offsets = bld.vgrf(BRW_TYPE_UD);
         bld.MUL(per_slot_offsets, gs_vertex_count,
                 brw_imm_ud(output_vertex_size));
      }
   }

   length = 0;
   urb_offset = starting_urb_offset;
   flush = false;

   /* SSO shaders can have VUE slots allocated which are never actually
    * written to, so ignore them when looking for the last (written) slot.
    */
   int last_slot = vue_map->num_slots - 1;
   while (last_slot > 0 &&
          (vue_map->slot_to_varying[last_slot] == BRW_VARYING_SLOT_PAD ||
           outputs[vue_map->slot_to_varying[last_slot]].file == BAD_FILE)) {
      last_slot--;
   }

   bool urb_written = false;
   for (slot = 0; slot < vue_map->num_slots; slot++) {
      int varying = vue_map->slot_to_varying[slot];
      switch (varying) {
      case VARYING_SLOT_PSIZ: {
         /* The point size varying slot is the vue header and is always in the
          * vue map.  But often none of the special varyings that live there
          * are written and in that case we can skip writing to the vue
          * header, provided the corresponding state properly clamps the
          * values further down the pipeline. */
         if ((vue_map->slots_valid & psiz_mask) == 0) {
            assert(length == 0);
            urb_offset++;
            break;
         }

         brw_reg zero = brw_vgrf(alloc.allocate(dispatch_width / 8),
                                BRW_TYPE_UD);
         bld.MOV(zero, brw_imm_ud(0u));

         if (vue_map->slots_valid & VARYING_BIT_PRIMITIVE_SHADING_RATE &&
             this->outputs[VARYING_SLOT_PRIMITIVE_SHADING_RATE].file != BAD_FILE) {
            sources[length++] = this->outputs[VARYING_SLOT_PRIMITIVE_SHADING_RATE];
         } else if (devinfo->has_coarse_pixel_primitive_and_cb) {
            uint32_t one_fp16 = 0x3C00;
            brw_reg one_by_one_fp16 = brw_vgrf(alloc.allocate(dispatch_width / 8),
                                              BRW_TYPE_UD);
            bld.MOV(one_by_one_fp16, brw_imm_ud((one_fp16 << 16) | one_fp16));
            sources[length++] = one_by_one_fp16;
         } else {
            sources[length++] = zero;
         }

         if (vue_map->slots_valid & VARYING_BIT_LAYER)
            sources[length++] = this->outputs[VARYING_SLOT_LAYER];
         else
            sources[length++] = zero;

         if (vue_map->slots_valid & VARYING_BIT_VIEWPORT)
            sources[length++] = this->outputs[VARYING_SLOT_VIEWPORT];
         else
            sources[length++] = zero;

         if (vue_map->slots_valid & VARYING_BIT_PSIZ)
            sources[length++] = this->outputs[VARYING_SLOT_PSIZ];
         else
            sources[length++] = zero;
         break;
      }
      case VARYING_SLOT_EDGE:
         unreachable("unexpected scalar vs output");
         break;

      default:
         /* gl_Position is always in the vue map, but isn't always written by
          * the shader.  Other varyings (clip distances) get added to the vue
          * map but don't always get written.  In those cases, the
          * corresponding this->output[] slot will be invalid we and can skip
          * the urb write for the varying.  If we've already queued up a vue
          * slot for writing we flush a mlen 5 urb write, otherwise we just
          * advance the urb_offset.
          */
         if (varying == BRW_VARYING_SLOT_PAD ||
             this->outputs[varying].file == BAD_FILE) {
            if (length > 0)
               flush = true;
            else
               urb_offset++;
            break;
         }

         int slot_offset = 0;

         /* When using Primitive Replication, there may be multiple slots
          * assigned to POS.
          */
         if (varying == VARYING_SLOT_POS)
            slot_offset = slot - vue_map->varying_to_slot[VARYING_SLOT_POS];

         for (unsigned i = 0; i < 4; i++) {
            sources[length++] = offset(this->outputs[varying], bld,
                                       i + (slot_offset * 4));
         }
         break;
      }

      const fs_builder abld = bld.annotate("URB write");

      /* If we've queued up 8 registers of payload (2 VUE slots), if this is
       * the last slot or if we need to flush (see BAD_FILE varying case
       * above), emit a URB write send now to flush out the data.
       */
      if (length == 8 || (length > 0 && slot == last_slot))
         flush = true;
      if (flush) {
         brw_reg srcs[URB_LOGICAL_NUM_SRCS];

         srcs[URB_LOGICAL_SRC_HANDLE] = urb_handle;
         srcs[URB_LOGICAL_SRC_PER_SLOT_OFFSETS] = per_slot_offsets;
         srcs[URB_LOGICAL_SRC_DATA] = brw_vgrf(alloc.allocate((dispatch_width / 8) * length),
                                               BRW_TYPE_F);
         srcs[URB_LOGICAL_SRC_COMPONENTS] = brw_imm_ud(length);
         abld.LOAD_PAYLOAD(srcs[URB_LOGICAL_SRC_DATA], sources, length, 0);

         fs_inst *inst = abld.emit(SHADER_OPCODE_URB_WRITE_LOGICAL, reg_undef,
                                   srcs, ARRAY_SIZE(srcs));

         /* For Wa_1805992985 one needs additional write in the end. */
         if (intel_needs_workaround(devinfo, 1805992985) && stage == MESA_SHADER_TESS_EVAL)
            inst->eot = false;
         else
            inst->eot = slot == last_slot && stage != MESA_SHADER_GEOMETRY;

         inst->offset = urb_offset;
         urb_offset = starting_urb_offset + slot + 1;
         length = 0;
         flush = false;
         urb_written = true;
      }
   }

   /* If we don't have any valid slots to write, just do a minimal urb write
    * send to terminate the shader.  This includes 1 slot of undefined data,
    * because it's invalid to write 0 data:
    *
    * From the Broadwell PRM, Volume 7: 3D Media GPGPU, Shared Functions -
    * Unified Return Buffer (URB) > URB_SIMD8_Write and URB_SIMD8_Read >
    * Write Data Payload:
    *
    *    "The write data payload can be between 1 and 8 message phases long."
    */
   if (!urb_written) {
      /* For GS, just turn EmitVertex() into a no-op.  We don't want it to
       * end the thread, and emit_gs_thread_end() already emits a SEND with
       * EOT at the end of the program for us.
       */
      if (stage == MESA_SHADER_GEOMETRY)
         return;

      brw_reg uniform_urb_handle = brw_vgrf(alloc.allocate(dispatch_width / 8),
                                           BRW_TYPE_UD);
      brw_reg payload = brw_vgrf(alloc.allocate(dispatch_width / 8),
                                BRW_TYPE_UD);

      bld.exec_all().MOV(uniform_urb_handle, urb_handle);

      brw_reg srcs[URB_LOGICAL_NUM_SRCS];
      srcs[URB_LOGICAL_SRC_HANDLE] = uniform_urb_handle;
      srcs[URB_LOGICAL_SRC_DATA] = payload;
      srcs[URB_LOGICAL_SRC_COMPONENTS] = brw_imm_ud(1);

      fs_inst *inst = bld.emit(SHADER_OPCODE_URB_WRITE_LOGICAL, reg_undef,
                               srcs, ARRAY_SIZE(srcs));
      inst->eot = true;
      inst->offset = 1;
      return;
   }

   /* Wa_1805992985:
    *
    * GPU hangs on one of tessellation vkcts tests with DS not done. The
    * send cycle, which is a urb write with an eot must be 4 phases long and
    * all 8 lanes must valid.
    */
   if (intel_needs_workaround(devinfo, 1805992985) && stage == MESA_SHADER_TESS_EVAL) {
      assert(dispatch_width == 8);
      brw_reg uniform_urb_handle = brw_vgrf(alloc.allocate(1), BRW_TYPE_UD);
      brw_reg uniform_mask = brw_vgrf(alloc.allocate(1), BRW_TYPE_UD);
      brw_reg payload = brw_vgrf(alloc.allocate(4), BRW_TYPE_UD);

      /* Workaround requires all 8 channels (lanes) to be valid. This is
       * understood to mean they all need to be alive. First trick is to find
       * a live channel and copy its urb handle for all the other channels to
       * make sure all handles are valid.
       */
      bld.exec_all().MOV(uniform_urb_handle, bld.emit_uniformize(urb_handle));

      /* Second trick is to use masked URB write where one can tell the HW to
       * actually write data only for selected channels even though all are
       * active.
       * Third trick is to take advantage of the must-be-zero (MBZ) area in
       * the very beginning of the URB.
       *
       * One masks data to be written only for the first channel and uses
       * offset zero explicitly to land data to the MBZ area avoiding trashing
       * any other part of the URB.
       *
       * Since the WA says that the write needs to be 4 phases long one uses
       * 4 slots data. All are explicitly zeros in order to to keep the MBZ
       * area written as zeros.
       */
      bld.exec_all().MOV(uniform_mask, brw_imm_ud(0x10000u));
      bld.exec_all().MOV(offset(payload, bld, 0), brw_imm_ud(0u));
      bld.exec_all().MOV(offset(payload, bld, 1), brw_imm_ud(0u));
      bld.exec_all().MOV(offset(payload, bld, 2), brw_imm_ud(0u));
      bld.exec_all().MOV(offset(payload, bld, 3), brw_imm_ud(0u));

      brw_reg srcs[URB_LOGICAL_NUM_SRCS];
      srcs[URB_LOGICAL_SRC_HANDLE] = uniform_urb_handle;
      srcs[URB_LOGICAL_SRC_CHANNEL_MASK] = uniform_mask;
      srcs[URB_LOGICAL_SRC_DATA] = payload;
      srcs[URB_LOGICAL_SRC_COMPONENTS] = brw_imm_ud(4);

      fs_inst *inst = bld.exec_all().emit(SHADER_OPCODE_URB_WRITE_LOGICAL,
                                          reg_undef, srcs, ARRAY_SIZE(srcs));
      inst->eot = true;
      inst->offset = 0;
   }
}

void
fs_visitor::emit_urb_fence()
{
   const fs_builder bld = fs_builder(this).at_end();
   brw_reg dst = bld.vgrf(BRW_TYPE_UD);
   fs_inst *fence = bld.emit(SHADER_OPCODE_MEMORY_FENCE, dst,
                             brw_vec8_grf(0, 0),
                             brw_imm_ud(true),
                             brw_imm_ud(0));
   fence->sfid = BRW_SFID_URB;
   fence->desc = lsc_fence_msg_desc(devinfo, LSC_FENCE_LOCAL,
                                    LSC_FLUSH_TYPE_NONE, true);

   bld.exec_all().group(1, 0).emit(FS_OPCODE_SCHEDULING_FENCE,
                                   bld.null_reg_ud(),
                                   &dst,
                                   1);
}

void
fs_visitor::emit_cs_terminate()
{
   const fs_builder ubld = fs_builder(this).at_end().exec_all();

   /* We can't directly send from g0, since sends with EOT have to use
    * g112-127. So, copy it to a virtual register, The register allocator will
    * make sure it uses the appropriate register range.
    */
   struct brw_reg g0 = retype(brw_vec8_grf(0, 0), BRW_TYPE_UD);
   brw_reg payload = brw_vgrf(alloc.allocate(reg_unit(devinfo)),
                             BRW_TYPE_UD);
   ubld.group(8 * reg_unit(devinfo), 0).MOV(payload, g0);

   /* Set the descriptor to "Dereference Resource" and "Root Thread" */
   unsigned desc = 0;

   /* Set Resource Select to "Do not dereference URB" on Gfx < 11.
    *
    * Note that even though the thread has a URB resource associated with it,
    * we set the "do not dereference URB" bit, because the URB resource is
    * managed by the fixed-function unit, so it will free it automatically.
    */
   if (devinfo->ver < 11)
      desc |= (1 << 4); /* Do not dereference URB */

   brw_reg srcs[4] = {
      brw_imm_ud(desc), /* desc */
      brw_imm_ud(0), /* ex_desc */
      payload,       /* payload */
      brw_reg(),      /* payload2 */
   };

   fs_inst *send = ubld.emit(SHADER_OPCODE_SEND, reg_undef, srcs, 4);

   /* On Alchemist and later, send an EOT message to the message gateway to
    * terminate a compute shader.  For older GPUs, send to the thread spawner.
    */
   send->sfid = devinfo->verx10 >= 125 ? BRW_SFID_MESSAGE_GATEWAY
                                       : BRW_SFID_THREAD_SPAWNER;
   send->mlen = reg_unit(devinfo);
   send->eot = true;
}

fs_visitor::fs_visitor(const struct brw_compiler *compiler,
                       const struct brw_compile_params *params,
                       const brw_base_prog_key *key,
                       struct brw_stage_prog_data *prog_data,
                       const nir_shader *shader,
                       unsigned dispatch_width,
                       bool needs_register_pressure,
                       bool debug_enabled)
   : compiler(compiler), log_data(params->log_data),
     devinfo(compiler->devinfo), nir(shader),
     mem_ctx(params->mem_ctx),
     cfg(NULL), stage(shader->info.stage),
     debug_enabled(debug_enabled),
     key(key), gs_compile(NULL), prog_data(prog_data),
     live_analysis(this), regpressure_analysis(this),
     performance_analysis(this), idom_analysis(this), def_analysis(this),
     needs_register_pressure(needs_register_pressure),
     dispatch_width(dispatch_width),
     max_polygons(0),
     api_subgroup_size(brw_nir_api_subgroup_size(shader, dispatch_width))
{
   init();
}

fs_visitor::fs_visitor(const struct brw_compiler *compiler,
                       const struct brw_compile_params *params,
                       const brw_wm_prog_key *key,
                       struct brw_wm_prog_data *prog_data,
                       const nir_shader *shader,
                       unsigned dispatch_width, unsigned max_polygons,
                       bool needs_register_pressure,
                       bool debug_enabled)
   : compiler(compiler), log_data(params->log_data),
     devinfo(compiler->devinfo), nir(shader),
     mem_ctx(params->mem_ctx),
     cfg(NULL), stage(shader->info.stage),
     debug_enabled(debug_enabled),
     key(&key->base), gs_compile(NULL), prog_data(&prog_data->base),
     live_analysis(this), regpressure_analysis(this),
     performance_analysis(this), idom_analysis(this), def_analysis(this),
     needs_register_pressure(needs_register_pressure),
     dispatch_width(dispatch_width),
     max_polygons(max_polygons),
     api_subgroup_size(brw_nir_api_subgroup_size(shader, dispatch_width))
{
   init();
   assert(api_subgroup_size == 0 ||
          api_subgroup_size == 8 ||
          api_subgroup_size == 16 ||
          api_subgroup_size == 32);
}

fs_visitor::fs_visitor(const struct brw_compiler *compiler,
                       const struct brw_compile_params *params,
                       struct brw_gs_compile *c,
                       struct brw_gs_prog_data *prog_data,
                       const nir_shader *shader,
                       bool needs_register_pressure,
                       bool debug_enabled)
   : compiler(compiler), log_data(params->log_data),
     devinfo(compiler->devinfo), nir(shader),
     mem_ctx(params->mem_ctx),
     cfg(NULL), stage(shader->info.stage),
     debug_enabled(debug_enabled),
     key(&c->key.base), gs_compile(c),
     prog_data(&prog_data->base.base),
     live_analysis(this), regpressure_analysis(this),
     performance_analysis(this), idom_analysis(this), def_analysis(this),
     needs_register_pressure(needs_register_pressure),
     dispatch_width(compiler->devinfo->ver >= 20 ? 16 : 8),
     max_polygons(0),
     api_subgroup_size(brw_nir_api_subgroup_size(shader, dispatch_width))
{
   init();
   assert(api_subgroup_size == 0 ||
          api_subgroup_size == 8 ||
          api_subgroup_size == 16 ||
          api_subgroup_size == 32);
}

void
fs_visitor::init()
{
   this->max_dispatch_width = 32;

   this->failed = false;
   this->fail_msg = NULL;

   this->payload_ = NULL;
   this->source_depth_to_render_target = false;
   this->first_non_payload_grf = 0;

   this->uniforms = 0;
   this->last_scratch = 0;
   this->push_constant_loc = NULL;

   memset(&this->shader_stats, 0, sizeof(this->shader_stats));

   this->grf_used = 0;
   this->spilled_any_registers = false;
}

fs_visitor::~fs_visitor()
{
   delete this->payload_;
}
