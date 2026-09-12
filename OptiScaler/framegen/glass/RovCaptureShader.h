#pragma once
#include <sstream>
#include <stdexcept>
#include <string>

namespace GlassFg::Detail
{
// Only appended instrumentation is changed here, never the original PS body.
// Its original exports are retained by RewriteMaterialMotion.
inline std::string CaptureOriginalColor(std::string instrumentation, bool mapped = false, unsigned instanceMapId = 0,
                                       bool coverageOnly = false, bool auditCoverage = false)
{
    if (auditCoverage && !coverageOnly)
        throw std::runtime_error("Coverage audit requires coverage storage");
    if (coverageOnly && !mapped)
        throw std::runtime_error("Coverage requires object mapping");
    for (const auto* condition : { "bad", "empty", "nonfinite" })
    {
        const std::string discard = std::string("  call void @dx.op.discard(i32 82, i1 %glass.") + condition + ")\n";
        const auto at = instrumentation.find(discard);
        if (at != std::string::npos)
            instrumentation.erase(at, discard.size());
    }
    if (!coverageOnly)
    {
        const auto firstOutput = instrumentation.find("  call void @dx.op.storeOutput.f32");
        if (firstOutput == std::string::npos)
            throw std::runtime_error("Missing rewritten material output");
        instrumentation.erase(firstOutput);
    }
    if (mapped && !auditCoverage)
    {
        const auto mapInput = instrumentation.find("  %glass.mapindex =");
        if (mapInput == std::string::npos)
            throw std::runtime_error("Missing mapped material input");
        const auto afterInput = instrumentation.find('\n', mapInput);
        instrumentation.insert(afterInput + 1,
                               "  %glass.mappedactive = icmp ne i32 %glass.mapindex, -1\n"
                               "  br i1 %glass.mappedactive, label %glass.capturebegin, label %glass.captureend\n"
                               "glass.capturebegin:\n");
    }
    std::ostringstream code;
    if (auditCoverage)
    {
        // No map/history branch precedes these reference writes. Original
        // material discard and early depth still apply; original color survives.
        code << R"(  %glass.auditroi = call %dx.types.CBufRet.i32 @dx.op.cbufferLoadLegacy.i32(i32 59, %dx.types.Handle %glass.cb, i32 2)
  %glass.auditdest = call %dx.types.CBufRet.i32 @dx.op.cbufferLoadLegacy.i32(i32 59, %dx.types.Handle %glass.cb, i32 3)
)";
        for (unsigned c = 0; c < 4; ++c)
            code << "  %glass.ar" << c << " = extractvalue %dx.types.CBufRet.i32 %glass.auditroi, " << c << "\n"
                 << "  %glass.ad" << c << " = extractvalue %dx.types.CBufRet.i32 %glass.auditdest, " << c << "\n";
        code << R"(  %glass.ax = fptoui float %glass.s0 to i32
  %glass.ay = fptoui float %glass.s1 to i32
  %glass.arx = sub i32 %glass.ax, %glass.ar0
  %glass.ary = sub i32 %glass.ay, %glass.ar1
  %glass.axok = icmp ult i32 %glass.arx, %glass.ar2
  %glass.ayok = icmp ult i32 %glass.ary, %glass.ar3
  %glass.axyok = and i1 %glass.axok, %glass.ayok
  br i1 %glass.axyok, label %glass.auditaddress, label %glass.auditend
glass.auditaddress:
  %glass.arow = mul i32 %glass.ary, %glass.ad1
  %glass.apixel = add i32 %glass.arow, %glass.arx
  %glass.abit0 = add i32 %glass.apixel, %glass.ad0
  %glass.abit1 = add i32 %glass.apixel, %glass.ad3
  %glass.abound0 = icmp ult i32 %glass.abit0, %glass.ad2
  %glass.abound1 = icmp ult i32 %glass.abit1, %glass.ad2
  %glass.abound = and i1 %glass.abound0, %glass.abound1
  br i1 %glass.abound, label %glass.auditwrite, label %glass.auditend
glass.auditwrite:
  %glass.auditbuffer = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 1, i32 0, i32 1, i1 false)
  %glass.aword0 = lshr i32 %glass.abit0, 5
  %glass.aaddress0 = shl i32 %glass.aword0, 2
  %glass.ashift0 = and i32 %glass.abit0, 31
  %glass.amask0 = shl i32 1, %glass.ashift0
  %glass.aignored0 = call i32 @dx.op.atomicBinOp.i32(i32 78, %dx.types.Handle %glass.auditbuffer, i32 2, i32 %glass.aaddress0, i32 undef, i32 undef, i32 %glass.amask0)
  br i1 %glass.hasall, label %glass.auditcontribution, label %glass.auditend
glass.auditcontribution:
  %glass.aword1 = lshr i32 %glass.abit1, 5
  %glass.aaddress1 = shl i32 %glass.aword1, 2
  %glass.ashift1 = and i32 %glass.abit1, 31
  %glass.amask1 = shl i32 1, %glass.ashift1
  %glass.aignored1 = call i32 @dx.op.atomicBinOp.i32(i32 78, %dx.types.Handle %glass.auditbuffer, i32 2, i32 %glass.aaddress1, i32 undef, i32 undef, i32 %glass.amask1)
  br label %glass.auditend
glass.auditend:
  %glass.mappedactive = icmp ne i32 %glass.mapindex, -1
  br i1 %glass.mappedactive, label %glass.capturebegin, label %glass.captureend
glass.capturebegin:
)";
    }
    if (mapped)
        code << R"(  %glass.mapok = icmp ne i32 %glass.mapindex, -1
  %glass.recordok = and i1 %glass.mapok, %glass.hasall
)";
    else
        code << R"(  %glass.recordok0 = and i1 %glass.ok, %glass.finite
  %glass.recordok = and i1 %glass.recordok0, %glass.hasall
)";
    code << R"(
  br i1 %glass.recordok, label %glass.roibounds, label %glass.captureend
glass.roibounds:
)";
    if (mapped)
    {
        code << "  %glass.map = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 0, i32 " << instanceMapId
             << ", i32 1, i1 false)\n";
        code << R"(  %glass.mapaddress = shl i32 %glass.mapindex, 6
  %glass.roiaddress = or i32 %glass.mapaddress, 16
  %glass.destaddress = or i32 %glass.mapaddress, 32
  %glass.roi = call %dx.types.ResRet.i32 @dx.op.bufferLoad.i32(i32 68, %dx.types.Handle %glass.map, i32 %glass.roiaddress, i32 undef)
  %glass.dest = call %dx.types.ResRet.i32 @dx.op.bufferLoad.i32(i32 68, %dx.types.Handle %glass.map, i32 %glass.destaddress, i32 undef)
  %glass.capturebuffer = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 1, i32 0, i32 1, i1 false)
)";
    }
    else
        code << R"(
  %glass.roi = call %dx.types.CBufRet.i32 @dx.op.cbufferLoadLegacy.i32(i32 59, %dx.types.Handle %glass.cb, i32 2)
  %glass.dest = call %dx.types.CBufRet.i32 @dx.op.cbufferLoadLegacy.i32(i32 59, %dx.types.Handle %glass.cb, i32 3)
)";
    code << R"(
  %glass.stamp = call %dx.types.CBufRet.i32 @dx.op.cbufferLoadLegacy.i32(i32 59, %dx.types.Handle %glass.cb, i32 1)
)";
    const char* recordType = mapped ? "%dx.types.ResRet.i32" : "%dx.types.CBufRet.i32";
    for (unsigned c = 0; c < 4; ++c)
        code << "  %glass.roi" << c << " = extractvalue " << recordType << " %glass.roi, " << c << "\n"
             << "  %glass.dest" << c << " = extractvalue " << recordType << " %glass.dest, " << c << "\n";
    code << R"(  %glass.frame = extractvalue %dx.types.CBufRet.i32 %glass.stamp, 2
  %glass.reverse = extractvalue %dx.types.CBufRet.i32 %glass.stamp, 3
  %glass.x = fptoui float %glass.s0 to i32
  %glass.y = fptoui float %glass.s1 to i32
  %glass.rx = sub i32 %glass.x, %glass.roi0
  %glass.ry = sub i32 %glass.y, %glass.roi1
  %glass.xok = icmp ult i32 %glass.rx, %glass.roi2
  %glass.yok = icmp ult i32 %glass.ry, %glass.roi3
  %glass.xyok = and i1 %glass.xok, %glass.yok
)";
    if (mapped)
        code << R"(  %glass.flagmissing = select i1 %glass.ok, i32 0, i32 2
  %glass.flagfinite = select i1 %glass.finite, i32 0, i32 4
  %glass.flagescape = select i1 %glass.xyok, i32 0, i32 1
  %glass.flags0 = or i32 %glass.flagmissing, %glass.flagfinite
  %glass.flags = or i32 %glass.flags0, %glass.flagescape
  %glass.anyflag = icmp ne i32 %glass.flags, 0
  br i1 %glass.anyflag, label %glass.flag, label %glass.addresscheck
glass.flag:
  %glass.statusaddress = shl i32 %glass.dest3, 5
  %glass.ignored = call i32 @dx.op.atomicBinOp.i32(i32 78, %dx.types.Handle %glass.capturebuffer, i32 2, i32 %glass.statusaddress, i32 undef, i32 undef, i32 %glass.flags)
  br label %glass.captureend
)";
    else
        code << R"(
  br i1 %glass.xyok, label %glass.addresscheck, label %glass.captureend
)";
    code << R"(
glass.addresscheck:
  %glass.rowoffset = mul i32 %glass.ry, %glass.dest1
  %glass.pixeloffset = add i32 %glass.rowoffset, %glass.rx
  %glass.pixelindex = add i32 %glass.pixeloffset, %glass.dest0
  %glass.addressok = icmp ult i32 %glass.pixelindex, %glass.dest2
  br i1 %glass.addressok, label %glass.capture, label %glass.captureend
glass.capture:
)";
    if (!mapped)
        code << "  %glass.capturebuffer = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 1, i32 0, i32 1, i1 "
                "false)\n";
    code << R"(
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
    auto storage = code.str();
    if (coverageOnly)
    {
        // Coverage has no history dependency. Keep original material discard,
        // contribution, object mapping and bounds checks; emit no motion.
        const auto flags = storage.find("  %glass.flagmissing =");
        const auto escape = storage.find("  %glass.flagescape =", flags);
        storage.erase(flags, escape - flags);
        const auto combine = storage.find("  %glass.flags0 =");
        const auto any = storage.find("  %glass.anyflag =", combine);
        storage.replace(combine, any - combine, "  %glass.flags = or i32 %glass.flagescape, 0\n");
        const auto write = storage.find("  %glass.address = shl i32 %glass.pixelindex, 5");
        storage.erase(write);
        storage += R"(  %glass.word = lshr i32 %glass.pixelindex, 5
  %glass.address = shl i32 %glass.word, 2
  %glass.bit = and i32 %glass.pixelindex, 31
  %glass.bitmask = shl i32 1, %glass.bit
  %glass.coverageignored = call i32 @dx.op.atomicBinOp.i32(i32 78, %dx.types.Handle %glass.capturebuffer, i32 2, i32 %glass.address, i32 undef, i32 undef, i32 %glass.bitmask)
  br label %glass.captureend
glass.captureend:
  ret void)";
        // Coverage destinations/status are word indices, unlike 32-byte MV records.
        const auto status = storage.find("%glass.statusaddress = shl i32 %glass.dest3, 5");
        storage.replace(status, std::string("%glass.statusaddress = shl i32 %glass.dest3, 5").size(),
                        "%glass.statusaddress = shl i32 %glass.dest3, 2");
    }
    return instrumentation + storage;
}
} // namespace GlassFg::Detail
