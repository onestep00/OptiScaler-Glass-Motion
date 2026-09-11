struct Input { float4 position : SV_Position; float4 current : TEXCOORD0; float4 previous : TEXCOORD1; };
float4 main(Input i) : SV_Target { return float4(i.current.z,i.previous.z,0.75,1); }
