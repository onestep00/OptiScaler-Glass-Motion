float4 main(uint id : SV_VertexID) : SV_Position
{
    return float4(id == 1 ? 1 : -1, id == 2 ? 1 : -1, 0.5, 1);
}
