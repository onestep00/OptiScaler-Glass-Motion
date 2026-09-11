struct Input { float4 position : SV_Position; float4 current : TEXCOORD0; float4 previous : TEXCOORD1; };
#ifdef GLASS_TEST_MRT
struct Output { float4 a : SV_Target0; float4 b : SV_Target1; float4 c : SV_Target2; };
Output main(Input i) { Output o; o.a=float4(i.current.z,i.previous.z,0.75,1); o.b=float4(0.2,0.4,0.6,1); o.c=float4(0.9,0.7,0.3,1); return o; }
#else
float4 main(Input i) : SV_Target { return float4(i.current.z,i.previous.z,0.75,1); }
#endif
