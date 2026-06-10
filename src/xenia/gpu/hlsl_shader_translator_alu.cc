/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2024 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/hlsl_shader_translator.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/platform.h"
#include "xenia/gpu/dxbc_shader.h"
#include "xenia/gpu/gpu_flags.h"
#include "xenia/gpu/ucode.h"
#include "xenia/gpu/xenos.h"
#if XE_PLATFORM_WIN32
// DXC (HLSL->DXIL) is part of the Windows-only D3D12 backend.
#include "xenia/gpu/d3d12/dxc_compiler.h"
#endif  // XE_PLATFORM_WIN32

namespace xe {
namespace gpu {

void HlslShaderTranslator::EmitKill(const std::string& condition) {
  EmitLine("if (" + condition + ") {");
  Indent();
  if (memexport_eM_written_before_kill_) {
    // Flush exports issued before the kill while the lane is still active, then
    // clear the written mask so the end-of-shader flush can't re-export this
    // now-discarded lane. Mirrors DxbcShaderTranslator::KillPixel.
    EmitMemExportFlush();
    EmitLine("xe_eM_written = 0u;");
  }
  EmitLine("discard;");
  Outdent();
  EmitLine("}");
}

void HlslShaderTranslator::ProcessAluInstruction(
    const ParsedAluInstruction& instr,
    uint8_t memexport_eM_potentially_written_before) {
  // Exports that may be in flight when a kill in this instruction discards the
  // lane. EmitKill flushes them before the discard.
  memexport_eM_written_before_kill_ = memexport_eM_potentially_written_before;

  // Handle instruction predication.
  bool needs_predicate_close = false;
  if (instr.is_predicated) {
    EmitLine("if (xe_p0 " +
             std::string(instr.predicate_condition ? "==" : "!=") + " true) {");
    Indent();
    needs_predicate_close = true;
  }

  // Snapshot scalar operand sources before the co-issued vector op writes its
  // destination. On Xenos both ALU pipes read the pre-instruction register
  // file, so a scalar that reads a register the vector writes must see the old
  // value.
  if (instr.scalar_opcode != ucode::AluScalarOpcode::kRetainPrev) {
    if (instr.scalar_operand_count > 0) {
      EmitLine("xe_salu_src0 = " +
               OperandToHlslNoSwizzle(instr.scalar_operands[0]) + ";");
    }
    if (instr.scalar_operand_count > 1) {
      EmitLine("xe_salu_src1 = " + OperandToHlsl(instr.scalar_operands[1], 1) +
               ";");
    }
  }

  // Process vector operation.
  // Note: ProcessVectorAluInstruction calls StoreConstantComponents internally.
  ProcessVectorAluInstruction(instr);

  // Process scalar operation.
  ProcessScalarAluInstruction(instr);

  // Store any constant-only components for scalar results.
  // Vector constant components are handled inside ProcessVectorAluInstruction.
  StoreConstantComponents(instr.scalar_result);

  // Record color targets written on this path for the ROV output merger. Inside
  // the predicate block so a (p0) export only marks the targets it actually
  // wrote.
  MarkColorWrittenIfRov(instr.vector_and_constant_result);
  MarkColorWrittenIfRov(instr.scalar_result);

  if (needs_predicate_close) {
    Outdent();
    EmitLine("}");
  }
}

void HlslShaderTranslator::ProcessVectorAluInstruction(
    const ParsedAluInstruction& instr) {
  uint32_t used_result_components =
      instr.vector_and_constant_result.GetUsedResultComponents();
  if (!used_result_components &&
      !ucode::GetAluVectorOpcodeInfo(instr.vector_opcode).changed_state) {
    // Constant-only writes (like max oC0._001 setting alpha to 1) still emit,
    // matching DXBC's unconditional StoreResult after the vector op.
    StoreConstantComponents(instr.vector_and_constant_result);
    return;
  }

  // Get operands.
  std::string op0, op1, op2;
  if (instr.vector_operand_count > 0) {
    op0 = OperandToHlsl(instr.vector_operands[0], 4);
  }
  if (instr.vector_operand_count > 1) {
    op1 = OperandToHlsl(instr.vector_operands[1], 4);
  }
  if (instr.vector_operand_count > 2) {
    op2 = OperandToHlsl(instr.vector_operands[2], 4);
  }

  std::string result;
  std::string result_swizzle;  // For scalar results like dp4 that replicate

  using AluVectorOpcode = ucode::AluVectorOpcode;
  switch (instr.vector_opcode) {
    case AluVectorOpcode::kAdd:
      result = "(" + op0 + " + " + op1 + ")";
      break;

    case AluVectorOpcode::kMul:
      // SM3: 0 * anything = 0
      result = "XeMulSM3(" + op0 + ", " + op1 + ")";
      break;

    case AluVectorOpcode::kMad:
      // SM3: 0 * anything = 0, then add
      result = "(XeMulSM3(" + op0 + ", " + op1 + ") + " + op2 + ")";
      break;

    case AluVectorOpcode::kMax:
      // SM3 NaN behavior: a >= b ? a : b (not fmax)
      // Optimization: if both operands are identical, just use the operand
      // directly. This avoids a DXC optimizer bug where select(cond, a, a)
      // with fast-math enabled incorrectly replaces 0.0 values with 1.0.
      if (op0 == op1) {
        result = op0;
      } else {
        result =
            "select((" + op0 + " >= " + op1 + "), " + op0 + ", " + op1 + ")";
      }
      break;

    case AluVectorOpcode::kMin:
      // SM3 NaN behavior: a < b ? a : b (not fmin)
      // Optimization: if both operands are identical, just use the operand.
      if (op0 == op1) {
        result = op0;
      } else {
        result =
            "select((" + op0 + " < " + op1 + "), " + op0 + ", " + op1 + ")";
      }
      break;

    case AluVectorOpcode::kSeq:
      result = "select((" + op0 + " == " + op1 +
               "), float4(1.0, 1.0, 1.0, 1.0), float4(0.0, 0.0, 0.0, 0.0))";
      break;

    case AluVectorOpcode::kSgt:
      result = "select((" + op0 + " > " + op1 +
               "), float4(1.0, 1.0, 1.0, 1.0), float4(0.0, 0.0, 0.0, 0.0))";
      break;

    case AluVectorOpcode::kSge:
      result = "select((" + op0 + " >= " + op1 +
               "), float4(1.0, 1.0, 1.0, 1.0), float4(0.0, 0.0, 0.0, 0.0))";
      break;

    case AluVectorOpcode::kSne:
      result = "select((" + op0 + " != " + op1 +
               "), float4(1.0, 1.0, 1.0, 1.0), float4(0.0, 0.0, 0.0, 0.0))";
      break;

    case AluVectorOpcode::kFrc:
      result = "frac(" + op0 + ")";
      break;

    case AluVectorOpcode::kTrunc:
      result = "trunc(" + op0 + ")";
      break;

    case AluVectorOpcode::kFloor:
      result = "floor(" + op0 + ")";
      break;

    case AluVectorOpcode::kCndEq:
      result = "select((" + op0 + " == float4(0.0, 0.0, 0.0, 0.0)), " + op1 +
               ", " + op2 + ")";
      break;

    case AluVectorOpcode::kCndGe:
      result = "select((" + op0 + " >= float4(0.0, 0.0, 0.0, 0.0)), " + op1 +
               ", " + op2 + ")";
      break;

    case AluVectorOpcode::kCndGt:
      result = "select((" + op0 + " > float4(0.0, 0.0, 0.0, 0.0)), " + op1 +
               ", " + op2 + ")";
      break;

    case AluVectorOpcode::kDp4:
      // SM3 dot (per-term 0*x=0, no fused multiply-add) replicated to all
      // components.
      result = "(XeDotSM3(" + op0 + ", " + op1 + ")).xxxx";
      break;

    case AluVectorOpcode::kDp3: {
      std::string op0_xyz = OperandToHlsl(instr.vector_operands[0], 3);
      std::string op1_xyz = OperandToHlsl(instr.vector_operands[1], 3);
      result = "(XeDotSM3(" + op0_xyz + ", " + op1_xyz + ")).xxxx";
    } break;

    case AluVectorOpcode::kDp2Add: {
      std::string op0_xy = OperandToHlsl(instr.vector_operands[0], 2);
      std::string op1_xy = OperandToHlsl(instr.vector_operands[1], 2);
      // src2 swizzle component 0
      std::string op2_x = OperandToHlsl(instr.vector_operands[2], 1);
      result =
          "(XeDotSM3(" + op0_xy + ", " + op1_xy + ") + " + op2_x + ").xxxx";
    } break;

    case AluVectorOpcode::kCube: {
      // Cube map coordinate calculation.
      // Input is in z_xy order (.zzxy swizzle applied to operand).
      // Result is (T coord, S coord, 2*major axis, face ID).
      EmitLine("{");
      Indent();
      EmitLine("float3 xe_cube_src = " +
               OperandToHlsl(instr.vector_operands[0], 3) + ";");
      EmitLine("float xe_cube_x = " +
               OperandToHlsl(instr.vector_operands[0], 4) + ".z;");
      EmitLine("float xe_cube_y = " +
               OperandToHlsl(instr.vector_operands[0], 4) + ".w;");
      EmitLine("float xe_cube_z = " +
               OperandToHlsl(instr.vector_operands[0], 4) + ".x;");
      EmitLine("float xe_cube_abs_x = abs(xe_cube_x);");
      EmitLine("float xe_cube_abs_y = abs(xe_cube_y);");
      EmitLine("float xe_cube_abs_z = abs(xe_cube_z);");
      EmitLine("float4 xe_cube_result;");
      EmitLine(
          "if (xe_cube_abs_z >= xe_cube_abs_x && xe_cube_abs_z >= "
          "xe_cube_abs_y) {");
      Indent();
      EmitLine("// Z is major axis");
      EmitLine("xe_cube_result.x = -xe_cube_y;");
      EmitLine(
          "xe_cube_result.y = (xe_cube_z < 0.0) ? -xe_cube_x : xe_cube_x;");
      EmitLine("xe_cube_result.z = 2.0 * xe_cube_z;");
      EmitLine("xe_cube_result.w = (xe_cube_z < 0.0) ? 5.0 : 4.0;");
      Outdent();
      EmitLine("} else if (xe_cube_abs_y >= xe_cube_abs_x) {");
      Indent();
      EmitLine("// Y is major axis");
      EmitLine(
          "xe_cube_result.x = (xe_cube_y < 0.0) ? -xe_cube_z : xe_cube_z;");
      EmitLine("xe_cube_result.y = xe_cube_x;");
      EmitLine("xe_cube_result.z = 2.0 * xe_cube_y;");
      EmitLine("xe_cube_result.w = (xe_cube_y < 0.0) ? 3.0 : 2.0;");
      Outdent();
      EmitLine("} else {");
      Indent();
      EmitLine("// X is major axis");
      EmitLine("xe_cube_result.x = -xe_cube_y;");
      EmitLine(
          "xe_cube_result.y = (xe_cube_x < 0.0) ? xe_cube_z : -xe_cube_z;");
      EmitLine("xe_cube_result.z = 2.0 * xe_cube_x;");
      EmitLine("xe_cube_result.w = (xe_cube_x < 0.0) ? 1.0 : 0.0;");
      Outdent();
      EmitLine("}");
      // Store result.
      EmitVectorResultAssignment(instr.vector_and_constant_result,
                                 "xe_cube_result");
      StoreConstantComponents(instr.vector_and_constant_result);
      Outdent();
      EmitLine("}");
      // Return early - we handled the result store.
      return;
    }

    case AluVectorOpcode::kMax4:
      // Find maximum of all 4 components.
      result = "(max(max(" + op0 + ".x, " + op0 + ".y), max(" + op0 + ".z, " +
               op0 + ".w))).xxxx";
      break;

    case AluVectorOpcode::kSetpEqPush:
    case AluVectorOpcode::kSetpNePush:
    case AluVectorOpcode::kSetpGtPush:
    case AluVectorOpcode::kSetpGePush: {
      // These set the predicate and return a value.
      // predicate = comparison(src0.w, 0) && comparison2(src1.w, 0)
      // result.x = comparison(src0.x, 0) && comparison2(src1.x, 0) ? 0 : src0.x
      // + 1
      std::string cmp_op, cmp_op2;
      switch (instr.vector_opcode) {
        case AluVectorOpcode::kSetpEqPush:
          cmp_op = "==";
          cmp_op2 = "==";
          break;
        case AluVectorOpcode::kSetpNePush:
          cmp_op = "==";
          cmp_op2 = "!=";
          break;
        case AluVectorOpcode::kSetpGtPush:
          cmp_op = "==";
          cmp_op2 = ">";
          break;
        case AluVectorOpcode::kSetpGePush:
          cmp_op = "==";
          cmp_op2 = ">=";
          break;
        default:
          break;
      }
      EmitLine("xe_p0 = (" + op0 + ".w " + cmp_op + " 0.0) && (" + op1 + ".w " +
               cmp_op2 + " 0.0);");
      // The result is the scalar src0.x + 1 (or 0) replicated to every written
      // component, matching DXBC's kXXXX result swizzle. Adding 1 to the whole
      // op0 vector instead would put src0.y/z/w + 1 into y/z/w.
      result = "((((" + op0 + ".x " + cmp_op + " 0.0) && (" + op1 + ".x " +
               cmp_op2 +
               " 0.0)) ? "
               "0.0 : (" +
               op0 + ".x + 1.0))).xxxx";
    } break;

    case AluVectorOpcode::kKillEq:
    case AluVectorOpcode::kKillGt:
    case AluVectorOpcode::kKillGe:
    case AluVectorOpcode::kKillNe: {
      std::string cmp_op;
      switch (instr.vector_opcode) {
        case AluVectorOpcode::kKillEq:
          cmp_op = "==";
          break;
        case AluVectorOpcode::kKillGt:
          cmp_op = ">";
          break;
        case AluVectorOpcode::kKillGe:
          cmp_op = ">=";
          break;
        case AluVectorOpcode::kKillNe:
          cmp_op = "!=";
          break;
        default:
          break;
      }
      EmitLine("{");
      Indent();
      EmitLine("bool4 xe_kill_cmp = (" + op0 + " " + cmp_op + " " + op1 + ");");
      EmitKill("any(xe_kill_cmp)");
      Outdent();
      EmitLine("}");
      // Scalar result replicated to all components
      result = "(any(" + op0 + " " + cmp_op + " " + op1 +
               ") ? float4(1.0, 1.0, 1.0, 1.0) : float4(0.0, 0.0, 0.0, 0.0))";
    } break;

    case AluVectorOpcode::kDst:
      // dst instruction: dest = (1, src0.y*src1.y, src0.z, src1.w)
      result = "float4(1.0, XeMulSM3(" + op0 + ".y, " + op1 + ".y), " + op0 +
               ".z, " + op1 + ".w)";
      break;

    case AluVectorOpcode::kMaxA: {
      // Update address register and return max.
      EmitLine("xe_a0 = clamp(int(floor(" + op0 + ".w + 0.5)), -256, 255);");
      // Optimization: if both operands are identical, just use the operand.
      if (op0 == op1) {
        result = op0;
      } else {
        result =
            "select((" + op0 + " >= " + op1 + "), " + op0 + ", " + op1 + ")";
      }
    } break;

    default:
      // Unhandled opcode - emit as zero.
      XELOGW("HLSL: Unhandled vector opcode: {}", instr.vector_opcode_name);
      result = "float4(0.0, 0.0, 0.0, 0.0)";
      break;
  }

  // Store result with proper swizzle matching.
  if (!result.empty()) {
    EmitVectorResultAssignment(instr.vector_and_constant_result, result);
    // Store any constant components (k0 or k1) in the write mask.
    // For example, oC0.0y01 means X=0, Y=computed, Z=0, W=1.
    StoreConstantComponents(instr.vector_and_constant_result);
  }
}

void HlslShaderTranslator::ProcessScalarAluInstruction(
    const ParsedAluInstruction& instr) {
  using AluScalarOpcode = ucode::AluScalarOpcode;

  // kRetainPrev keeps xe_ps unchanged but still writes it to the scalar dest.
  if (instr.scalar_opcode == AluScalarOpcode::kRetainPrev) {
    EmitScalarResultAssignment(instr.scalar_result, "xe_ps.x");
    return;
  }

  // Get operands. Scalar ops take 1-2 operands.
  // The first operand has two components (a, b) accessed differently per op.
  std::string op0_a, op0_b, op1;
  if (instr.scalar_operand_count > 0) {
    // Get component a and b from the first operand.
    const auto& operand0 = instr.scalar_operands[0];
    SwizzleSource comp_a = operand0.components[0];
    SwizzleSource comp_b = operand0.components[1];
    // Pre-instruction snapshot, captured in ProcessAluInstruction.
    std::string base = "xe_salu_src0";

    // Apply modifiers.
    std::string base_mod = base;
    if (operand0.is_absolute_value) {
      base_mod = "abs(" + base + ")";
    }
    if (operand0.is_negated) {
      base_mod = "-(" + base_mod + ")";
    }

    op0_a = base_mod + "." + GetCharForSwizzle(comp_a);
    op0_b = base_mod + "." + GetCharForSwizzle(comp_b);
  }
  if (instr.scalar_operand_count > 1) {
    // Pre-instruction snapshot, captured in ProcessAluInstruction.
    op1 = "xe_salu_src1";
  }

  std::string result;

  switch (instr.scalar_opcode) {
    case AluScalarOpcode::kAdds:
      result = "(" + op0_a + " + " + op0_b + ")";
      break;

    case AluScalarOpcode::kAddsPrev:
      result = "(" + op0_a + " + xe_ps.x)";
      break;

    case AluScalarOpcode::kMuls:
      result = "XeMulSM3(" + op0_a + ", " + op0_b + ")";
      break;

    case AluScalarOpcode::kMulsPrev:
      result = "XeMulSM3(" + op0_a + ", xe_ps.x)";
      break;

    case AluScalarOpcode::kMulsPrev2:
      // Complex LIT emulation operation.
      result =
          "((xe_ps.x == -3.402823466e+38 || !isfinite(xe_ps.x) || "
          "!isfinite(" +
          op0_b + ") || " + op0_b +
          " <= 0.0) ? "
          "-3.402823466e+38 : XeMulSM3(" +
          op0_a + ", xe_ps.x))";
      break;

    case AluScalarOpcode::kMaxs:
      // SM3 NaN behavior: a >= b ? a : b
      // Optimization: if both operands are identical, just use the operand.
      if (op0_a == op0_b) {
        result = op0_a;
      } else {
        result = "((" + op0_a + " >= " + op0_b + ") ? " + op0_a + " : " +
                 op0_b + ")";
      }
      break;

    case AluScalarOpcode::kMins:
      // SM3 NaN behavior: a < b ? a : b
      // Optimization: if both operands are identical, just use the operand.
      if (op0_a == op0_b) {
        result = op0_a;
      } else {
        result =
            "((" + op0_a + " < " + op0_b + ") ? " + op0_a + " : " + op0_b + ")";
      }
      break;

    case AluScalarOpcode::kSeqs:
      result = "((" + op0_a + " == 0.0) ? 1.0 : 0.0)";
      break;

    case AluScalarOpcode::kSgts:
      result = "((" + op0_a + " > 0.0) ? 1.0 : 0.0)";
      break;

    case AluScalarOpcode::kSges:
      result = "((" + op0_a + " >= 0.0) ? 1.0 : 0.0)";
      break;

    case AluScalarOpcode::kSnes:
      result = "((" + op0_a + " != 0.0) ? 1.0 : 0.0)";
      break;

    case AluScalarOpcode::kFrcs:
      result = "frac(" + op0_a + ")";
      break;

    case AluScalarOpcode::kTruncs:
      result = "trunc(" + op0_a + ")";
      break;

    case AluScalarOpcode::kFloors:
      result = "floor(" + op0_a + ")";
      break;

    case AluScalarOpcode::kExp:
      result = "exp2(" + op0_a + ")";
      break;

    case AluScalarOpcode::kLogc:
      result = "((log2(" + op0_a + ") == -1.0/0.0) ? -3.402823466e+38 : log2(" +
               op0_a + "))";
      break;

    case AluScalarOpcode::kLog:
      result = "log2(" + op0_a + ")";
      break;

    case AluScalarOpcode::kRcpc:
      // Reciprocal with infinity clamped to FLT_MAX.
      result = "((abs(1.0 / " + op0_a +
               ") == 1.0/0.0) ? "
               "(sign(1.0 / " +
               op0_a + ") * 3.402823466e+38) : (1.0 / " + op0_a + "))";
      break;

    case AluScalarOpcode::kRcpf:
      // Reciprocal with infinity flushed to zero, keeping the sign.
      result = "((abs(1.0 / " + op0_a + ") == 1.0/0.0) ? (sign(1.0 / " + op0_a +
               ") * 0.0) : (1.0 / " + op0_a + "))";
      break;

    case AluScalarOpcode::kRcp:
      result = "(1.0 / " + op0_a + ")";
      break;

    case AluScalarOpcode::kRsqc:
      // Reciprocal square root with infinity clamped.
      result = "((abs(rsqrt(" + op0_a +
               ")) == 1.0/0.0) ? "
               "(sign(rsqrt(" +
               op0_a + ")) * 3.402823466e+38) : rsqrt(" + op0_a + "))";
      break;

    case AluScalarOpcode::kRsqf:
      // Reciprocal square root with infinity flushed.
      result = "((abs(rsqrt(" + op0_a + ")) == 1.0/0.0) ? 0.0 : rsqrt(" +
               op0_a + "))";
      break;

    case AluScalarOpcode::kRsq:
      result = "rsqrt(" + op0_a + ")";
      break;

    case AluScalarOpcode::kMaxAs:
      // Update address register (clamped 0-255) and return max.
      EmitLine("xe_a0 = clamp(int(floor(" + op0_a + " + 0.5)), 0, 255);");
      // Optimization: if both operands are identical, just use the operand.
      if (op0_a == op0_b) {
        result = op0_a;
      } else {
        result = "((" + op0_a + " >= " + op0_b + ") ? " + op0_a + " : " +
                 op0_b + ")";
      }
      break;

    case AluScalarOpcode::kMaxAsf:
      // Update address register (floored, clamped 0-255) and return max.
      EmitLine("xe_a0 = clamp(int(floor(" + op0_a + ")), 0, 255);");
      // Optimization: if both operands are identical, just use the operand.
      if (op0_a == op0_b) {
        result = op0_a;
      } else {
        result = "((" + op0_a + " >= " + op0_b + ") ? " + op0_a + " : " +
                 op0_b + ")";
      }
      break;

    case AluScalarOpcode::kSubs:
      result = "(" + op0_a + " - " + op0_b + ")";
      break;

    case AluScalarOpcode::kSubsPrev:
      result = "(" + op0_a + " - xe_ps.x)";
      break;

    case AluScalarOpcode::kSetpEq:
      EmitLine("xe_p0 = (" + op0_a + " == 0.0);");
      result = "(xe_p0 ? 0.0 : 1.0)";
      break;

    case AluScalarOpcode::kSetpNe:
      EmitLine("xe_p0 = (" + op0_a + " != 0.0);");
      result = "(xe_p0 ? 0.0 : 1.0)";
      break;

    case AluScalarOpcode::kSetpGt:
      EmitLine("xe_p0 = (" + op0_a + " > 0.0);");
      result = "(xe_p0 ? 0.0 : 1.0)";
      break;

    case AluScalarOpcode::kSetpGe:
      EmitLine("xe_p0 = (" + op0_a + " >= 0.0);");
      result = "(xe_p0 ? 0.0 : 1.0)";
      break;

    case AluScalarOpcode::kSetpInv:
      // Sets predicate to (src == 1.0), result is (src == 0.0 ? 1.0 : src)
      // unless pred true then 0.
      EmitLine("xe_p0 = (" + op0_a + " == 1.0);");
      result = "(xe_p0 ? 0.0 : ((" + op0_a + " == 0.0) ? 1.0 : " + op0_a + "))";
      break;

    case AluScalarOpcode::kSetpPop:
      // Decrements and sets predicate if <= 0. Use inline expression.
      EmitLine("xe_p0 = ((" + op0_a + " - 1.0) <= 0.0);");
      result = "(xe_p0 ? 0.0 : (" + op0_a + " - 1.0))";
      break;

    case AluScalarOpcode::kSetpClr:
      EmitLine("xe_p0 = false;");
      result = "3.402823466e+38";
      break;

    case AluScalarOpcode::kSetpRstr:
      EmitLine("xe_p0 = (" + op0_a + " == 0.0);");
      result = "(xe_p0 ? 0.0 : " + op0_a + ")";
      break;

    case AluScalarOpcode::kKillsEq:
      EmitKill(op0_a + " == 0.0");
      result = "((" + op0_a + " == 0.0) ? 1.0 : 0.0)";
      break;

    case AluScalarOpcode::kKillsGt:
      EmitKill(op0_a + " > 0.0");
      result = "((" + op0_a + " > 0.0) ? 1.0 : 0.0)";
      break;

    case AluScalarOpcode::kKillsGe:
      EmitKill(op0_a + " >= 0.0");
      result = "((" + op0_a + " >= 0.0) ? 1.0 : 0.0)";
      break;

    case AluScalarOpcode::kKillsNe:
      EmitKill(op0_a + " != 0.0");
      result = "((" + op0_a + " != 0.0) ? 1.0 : 0.0)";
      break;

    case AluScalarOpcode::kKillsOne:
      EmitKill(op0_a + " == 1.0");
      result = "((" + op0_a + " == 1.0) ? 1.0 : 0.0)";
      break;

    case AluScalarOpcode::kSqrt:
      result = "sqrt(" + op0_a + ")";
      break;

    case AluScalarOpcode::kSin:
      result = "sin(" + op0_a + ")";
      break;

    case AluScalarOpcode::kCos:
      result = "cos(" + op0_a + ")";
      break;

    case AluScalarOpcode::kMulsc0:
    case AluScalarOpcode::kMulsc1:
      result = "XeMulSM3(" + op0_a + ", " + op1 + ")";
      break;

    case AluScalarOpcode::kAddsc0:
    case AluScalarOpcode::kAddsc1:
      result = "(" + op0_a + " + " + op1 + ")";
      break;

    case AluScalarOpcode::kSubsc0:
    case AluScalarOpcode::kSubsc1:
      result = "(" + op0_a + " - " + op1 + ")";
      break;

    default:
      // Unhandled opcode.
      XELOGW("HLSL: Unhandled scalar opcode: {}", instr.scalar_opcode_name);
      result = "0.0";
      break;
  }

  // Update ps with the scalar result, then write the dest from ps.
  if (!result.empty()) {
    EmitLine("xe_ps = float4(" + result + ", " + result + ", " + result + ", " +
             result + ");");
    EmitScalarResultAssignment(instr.scalar_result, "xe_ps.x");
  }
}
}  // namespace gpu
}  // namespace xe
