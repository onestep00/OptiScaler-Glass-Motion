#pragma once
#include <stdexcept>
#include <string>
#include <string_view>

namespace GlassFg::Detail
{
// Append one global packed-layer write after the original material body. The
// original color exports and discard stay intact. Missing history, inactive
// mapping, empty contribution and invalid values skip only this extra write.
// opacity names the instrumentation's record opacity value: %glass.alpha, or
// %glass.opacity for a light-pass PS and for coverage-only capture.
inline std::string CapturePackedMotion(std::string instrumentation, unsigned instanceMapId, unsigned captureUavId,
                                       std::string_view opacity)
{
    for (const auto* condition : {"bad", "empty", "nonfinite"})
    {
        const std::string discard = std::string("  call void @dx.op.discard(i32 82, i1 %glass.") + condition + ")\n";
        const auto at = instrumentation.find(discard);
        if (at != std::string::npos)
            instrumentation.erase(at, discard.size());
    }
    const auto firstOutput = instrumentation.find("  call void @dx.op.storeOutput.f32");
    // Known blend equations build temporary MV/opacity color stores which are
    // removed before the original material return. Coverage-only fallback has
    // no temporary color target; it still provides every value used below.
    if (firstOutput != std::string::npos)
        instrumentation.erase(firstOutput);

    return instrumentation + R"(  %glass.depthfinite = call i1 @dx.op.isSpecialFloat.f32(i32 10, float %glass.s2)
  %glass.viewstamp = call %dx.types.CBufRet.i32 @dx.op.cbufferLoadLegacy.i32(i32 59, %dx.types.Handle %glass.cb, i32 1)
  %glass.farload = call %dx.types.CBufRet.i32 @dx.op.cbufferLoadLegacy.i32(i32 59, %dx.types.Handle %glass.cb, i32 3)
  %glass.farbits = extractvalue %dx.types.CBufRet.i32 %glass.farload, 3
  %glass.farcut = bitcast i32 %glass.farbits to float
  %glass.faron = fcmp ogt float %glass.farcut, 0.000000e+00
  %glass.farunder = fcmp olt float %glass.p3, %glass.farcut
  %glass.faroff = xor i1 %glass.faron, true
  %glass.farok = or i1 %glass.faroff, %glass.farunder
  %glass.recordok0 = and i1 %glass.ok, %glass.finite
  %glass.recordok1 = and i1 %glass.recordok0, %glass.depthfinite
  %glass.mapok = icmp ne i32 %glass.mapindex, -1
  %glass.recordok2 = and i1 %glass.recordok1, %glass.mapok
  %glass.recordok3 = and i1 %glass.recordok2, %glass.farok
  %glass.recordok = and i1 %glass.recordok3, %glass.hasall
  br i1 %glass.recordok, label %glass.packedbounds, label %glass.packedend
glass.packedbounds:
  %glass.roi = call %dx.types.CBufRet.i32 @dx.op.cbufferLoadLegacy.i32(i32 59, %dx.types.Handle %glass.cb, i32 2)
  %glass.dest = call %dx.types.CBufRet.i32 @dx.op.cbufferLoadLegacy.i32(i32 59, %dx.types.Handle %glass.cb, i32 3)
  %glass.left = extractvalue %dx.types.CBufRet.i32 %glass.roi, 0
  %glass.top = extractvalue %dx.types.CBufRet.i32 %glass.roi, 1
  %glass.width = extractvalue %dx.types.CBufRet.i32 %glass.roi, 2
  %glass.height = extractvalue %dx.types.CBufRet.i32 %glass.roi, 3
  %glass.base = extractvalue %dx.types.CBufRet.i32 %glass.dest, 0
  %glass.stride = extractvalue %dx.types.CBufRet.i32 %glass.dest, 1
  %glass.capacity = extractvalue %dx.types.CBufRet.i32 %glass.dest, 2
  %glass.x = fptoui float %glass.s0 to i32
  %glass.y = fptoui float %glass.s1 to i32
  %glass.rx = sub i32 %glass.x, %glass.left
  %glass.ry = sub i32 %glass.y, %glass.top
  %glass.xok = icmp ult i32 %glass.rx, %glass.width
  %glass.yok = icmp ult i32 %glass.ry, %glass.height
  %glass.xyok = and i1 %glass.xok, %glass.yok
  br i1 %glass.xyok, label %glass.packedaddress, label %glass.packedend
glass.packedaddress:
  %glass.rowoffset = mul i32 %glass.ry, %glass.stride
  %glass.pixeloffset = add i32 %glass.rowoffset, %glass.rx
  %glass.pixelindex = add i32 %glass.pixeloffset, %glass.base
  %glass.addressok = icmp ult i32 %glass.pixelindex, %glass.capacity
  br i1 %glass.addressok, label %glass.packedvalue, label %glass.packedend
glass.packedvalue:
  %glass.map = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 0, i32 )" +
           std::to_string(instanceMapId) + R"(, i32 1, i1 false)
  %glass.mapaddress = shl i32 %glass.mapindex, 6
  %glass.idaddress = or i32 %glass.mapaddress, 48
  %glass.idrecord = call %dx.types.ResRet.i32 @dx.op.rawBufferLoad.i32(i32 139, %dx.types.Handle %glass.map, i32 %glass.idaddress, i32 undef, i8 1, i32 4)
  %glass.objectid = extractvalue %dx.types.ResRet.i32 %glass.idrecord, 0
  %glass.idnonzero = icmp ne i32 %glass.objectid, 0
  %glass.idinrange = icmp ule i32 %glass.objectid, 32767
  %glass.idok = and i1 %glass.idnonzero, %glass.idinrange
  br i1 %glass.idok, label %glass.packedencode, label %glass.packedend
glass.packedencode:
  %glass.mxpixels = fdiv float %glass.mv0, %glass.v2
  %glass.mypixels = fdiv float %glass.mv1, %glass.v3
  %glass.mxscaled = fmul float %glass.mxpixels, 8.000000e+00
  %glass.myscaled = fmul float %glass.mypixels, 8.000000e+00
  %glass.mxlowerok = fcmp oge float %glass.mxscaled, -1.024000e+03
  %glass.mxupperok = fcmp ole float %glass.mxscaled, 1.023000e+03
  %glass.mylowerok = fcmp oge float %glass.myscaled, -1.024000e+03
  %glass.myupperok = fcmp ole float %glass.myscaled, 1.023000e+03
  %glass.mxrangeok = and i1 %glass.mxlowerok, %glass.mxupperok
  %glass.myrangeok = and i1 %glass.mylowerok, %glass.myupperok
  %glass.motionrangeok = and i1 %glass.mxrangeok, %glass.myrangeok
  br i1 %glass.motionrangeok, label %glass.packedquantize, label %glass.packedend
glass.packedquantize:
  %glass.mxi = fptosi float %glass.mxscaled to i32
  %glass.myi = fptosi float %glass.myscaled to i32
  %glass.mxbits = and i32 %glass.mxi, 2047
  %glass.mybits = and i32 %glass.myi, 2047
  ; The record opacity d: the background can change at most 1 - d of the
  ; displayed pixel. The 8-bit weight and the covered class below both come
  ; from this one value.
  %glass.alow = fcmp olt float )" +
           std::string(opacity) + R"(, 0.000000e+00
  %glass.alower = select i1 %glass.alow, float 0.000000e+00, float )" +
           std::string(opacity) + R"(
  %glass.ahigh = fcmp ogt float %glass.alower, 1.000000e+00
  %glass.aclamped = select i1 %glass.ahigh, float 1.000000e+00, float %glass.alower
  %glass.ascaled = fmul float %glass.aclamped, 2.550000e+02
  %glass.weight = fptoui float %glass.ascaled to i32
  %glass.dlow = fcmp olt float %glass.s2, 0.000000e+00
  %glass.dlower = select i1 %glass.dlow, float 0.000000e+00, float %glass.s2
  %glass.dhigh = fcmp ogt float %glass.dlower, 1.000000e+00
  %glass.dclamped = select i1 %glass.dhigh, float 1.000000e+00, float %glass.dlower
  ; 17 bits of depth leave the top bit of the 18-bit high key free for the
  ; covered/uncovered class. The store is an unsigned max, so a covered record
  ; always outranks an uncovered one and the nearest wins inside each class.
  ; One 8-byte record per pixel therefore resolves any number of overlapping
  ; transparent layers: the nearest surface that the visible-border/opacity
  ; rule keeps wins, and a nearly transparent layer never hides a visible one
  ; behind it.
  %glass.dscaled = fmul float %glass.dclamped, 1.310710e+05
  %glass.depth = fptoui float %glass.dscaled to i32
  %glass.reverse = extractvalue %dx.types.CBufRet.i32 %glass.viewstamp, 3
  %glass.forwarddepth = sub i32 131071, %glass.depth
  %glass.isreverse = icmp ne i32 %glass.reverse, 0
  %glass.depthkey = select i1 %glass.isreverse, i32 %glass.depth, i32 %glass.forwarddepth
  %glass.thresholdload = call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59, %dx.types.Handle %glass.cb, i32 4)
  %glass.threshold = extractvalue %dx.types.CBufRet.f32 %glass.thresholdload, 0
  %glass.weightf = uitofp i32 %glass.weight to float
  ; Exact float value of 1/255: LLVM IR rejects decimals that are not
  ; representable in the destination type without rounding.
  %glass.weightnorm = fmul float %glass.weightf, 3.9215688593685626983642578125e-03
  %glass.coverok = fcmp oge float %glass.aclamped, %glass.threshold
  %glass.coverbit = select i1 %glass.coverok, i32 131072, i32 0
  %glass.key = or i32 %glass.depthkey, %glass.coverbit
  %glass.depth64 = zext i32 %glass.key to i64
  %glass.mx64 = zext i32 %glass.mxbits to i64
  %glass.my64 = zext i32 %glass.mybits to i64
  %glass.weight64 = zext i32 %glass.weight to i64
  %glass.idmasked = and i32 %glass.objectid, 32767
  %glass.reversebit = select i1 %glass.isreverse, i32 32768, i32 0
  %glass.idflags = or i32 %glass.idmasked, %glass.reversebit
  %glass.id64 = zext i32 %glass.idflags to i64
  %glass.depthshift = shl i64 %glass.depth64, 46
  %glass.mxshift = shl i64 %glass.mx64, 35
  %glass.myshift = shl i64 %glass.my64, 24
  %glass.weightshift = shl i64 %glass.weight64, 16
  %glass.pack0 = or i64 %glass.depthshift, %glass.mxshift
  %glass.pack1 = or i64 %glass.pack0, %glass.myshift
  %glass.pack2 = or i64 %glass.pack1, %glass.weightshift
  %glass.packed = or i64 %glass.pack2, %glass.id64
  %glass.capturebuffer = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 1, i32 )" +
           std::to_string(captureUavId) + R"(, i32 1, i1 false)
  %glass.byteaddress = shl i32 %glass.pixelindex, 3
  %glass.oldpacked = call i64 @dx.op.atomicBinOp.i64(i32 78, %dx.types.Handle %glass.capturebuffer, i32 7, i32 %glass.byteaddress, i32 undef, i32 undef, i64 %glass.packed)
  br label %glass.packedend
glass.packedend:
  ret void)";
}
} // namespace GlassFg::Detail
