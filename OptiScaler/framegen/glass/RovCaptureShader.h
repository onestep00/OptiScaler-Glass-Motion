#pragma once
#include <sstream>
#include <string>

namespace GlassFg::Detail
{
// Only appended instrumentation is changed here, never the original PS body.
// Its original exports are retained by RewriteMaterialMotion.
inline std::string CaptureOriginalColor(std::string instrumentation)
{
    for (const auto* condition : { "bad", "empty", "nonfinite" })
    {
        const std::string discard = std::string("  call void @dx.op.discard(i32 82, i1 %glass.") + condition + ")\n";
        const auto at = instrumentation.find(discard);
        if (at != std::string::npos)
            instrumentation.erase(at, discard.size());
    }
    instrumentation.erase(instrumentation.find("  call void @dx.op.storeOutput.f32"));
    std::ostringstream code;
    code << R"(  %glass.recordok0 = and i1 %glass.ok, %glass.finite
  %glass.recordok = and i1 %glass.recordok0, %glass.hasall
  br i1 %glass.recordok, label %glass.roibounds, label %glass.captureend
glass.roibounds:
  %glass.roi = call %dx.types.CBufRet.i32 @dx.op.cbufferLoadLegacy.i32(i32 59, %dx.types.Handle %glass.cb, i32 2)
  %glass.dest = call %dx.types.CBufRet.i32 @dx.op.cbufferLoadLegacy.i32(i32 59, %dx.types.Handle %glass.cb, i32 3)
  %glass.stamp = call %dx.types.CBufRet.i32 @dx.op.cbufferLoadLegacy.i32(i32 59, %dx.types.Handle %glass.cb, i32 1)
)";
    for (unsigned c = 0; c < 4; ++c)
        code << "  %glass.roi" << c << " = extractvalue %dx.types.CBufRet.i32 %glass.roi, " << c << "\n"
             << "  %glass.dest" << c << " = extractvalue %dx.types.CBufRet.i32 %glass.dest, " << c << "\n";
    code << R"(  %glass.frame = extractvalue %dx.types.CBufRet.i32 %glass.stamp, 2
  %glass.reverse = extractvalue %dx.types.CBufRet.i32 %glass.stamp, 3
  %glass.x = fptoui float %glass.s0 to i32
  %glass.y = fptoui float %glass.s1 to i32
  %glass.rx = sub i32 %glass.x, %glass.roi0
  %glass.ry = sub i32 %glass.y, %glass.roi1
  %glass.xok = icmp ult i32 %glass.rx, %glass.roi2
  %glass.yok = icmp ult i32 %glass.ry, %glass.roi3
  %glass.xyok = and i1 %glass.xok, %glass.yok
  br i1 %glass.xyok, label %glass.addresscheck, label %glass.captureend
glass.addresscheck:
  %glass.rowoffset = mul i32 %glass.ry, %glass.dest1
  %glass.pixeloffset = add i32 %glass.rowoffset, %glass.rx
  %glass.pixelindex = add i32 %glass.pixeloffset, %glass.dest0
  %glass.addressok = icmp ult i32 %glass.pixelindex, %glass.dest2
  br i1 %glass.addressok, label %glass.capture, label %glass.captureend
glass.capture:
  %glass.capturebuffer = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 1, i32 0, i32 1, i1 false)
  %glass.address = shl i32 %glass.pixelindex, 5
  %glass.transaddress = or i32 %glass.address, 16
  %glass.old = call %dx.types.ResRet.i32 @dx.op.bufferLoad.i32(i32 68, %dx.types.Handle %glass.capturebuffer, i32 %glass.address, i32 undef)
  %glass.oldtrans = call %dx.types.ResRet.i32 @dx.op.bufferLoad.i32(i32 68, %dx.types.Handle %glass.capturebuffer, i32 %glass.transaddress, i32 undef)
  %glass.oldframe = extractvalue %dx.types.ResRet.i32 %glass.old, 3
  %glass.olddepthbits = extractvalue %dx.types.ResRet.i32 %glass.old, 2
  %glass.olddepth = bitcast i32 %glass.olddepthbits to float
  %glass.same = icmp eq i32 %glass.oldframe, %glass.frame
  %glass.nearer = fcmp olt float %glass.s2, %glass.olddepth
  %glass.nearerreverse = fcmp ogt float %glass.s2, %glass.olddepth
  %glass.isreverse = icmp ne i32 %glass.reverse, 0
  %glass.isnearer = select i1 %glass.isreverse, i1 %glass.nearerreverse, i1 %glass.nearer
  %glass.fresh = xor i1 %glass.same, true
  %glass.replace = or i1 %glass.fresh, %glass.isnearer
)";
    const char* current[] = { "%glass.mv0", "%glass.mv1", "%glass.s2" };
    for (unsigned c = 0; c < 3; ++c)
        code << "  %glass.oldbits" << c << " = extractvalue %dx.types.ResRet.i32 %glass.old, " << c << "\n"
             << "  %glass.newbits" << c << " = bitcast float " << current[c] << " to i32\n"
             << "  %glass.chosen" << c << " = select i1 %glass.replace, i32 %glass.newbits" << c
             << ", i32 %glass.oldbits" << c << "\n"
             << "  %glass.oldtbits" << c << " = extractvalue %dx.types.ResRet.i32 %glass.oldtrans, " << c << "\n"
             << "  %glass.oldt" << c << " = bitcast i32 %glass.oldtbits" << c << " to float\n"
             << "  %glass.startt" << c << " = select i1 %glass.same, float %glass.oldt" << c << ", float 1.000000e+00\n"
             << "  %glass.product" << c << " = fmul float %glass.startt" << c << ", %glass.t" << c << "\n"
             << "  %glass.productbits" << c << " = bitcast float %glass.product" << c << " to i32\n";
    code
        << R"(  call void @dx.op.bufferStore.i32(i32 69, %dx.types.Handle %glass.capturebuffer, i32 %glass.address, i32 undef, i32 %glass.chosen0, i32 %glass.chosen1, i32 %glass.chosen2, i32 %glass.frame, i8 15)
  call void @dx.op.bufferStore.i32(i32 69, %dx.types.Handle %glass.capturebuffer, i32 %glass.transaddress, i32 undef, i32 %glass.productbits0, i32 %glass.productbits1, i32 %glass.productbits2, i32 0, i8 15)
  br label %glass.captureend
glass.captureend:
  ret void)";
    return instrumentation + code.str();
}
} // namespace GlassFg::Detail
