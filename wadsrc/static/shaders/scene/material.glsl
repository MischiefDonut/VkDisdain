
#ifndef SIMPLE3D
	#include "shaders/scene/material_gettexel.glsl"
	#include "shaders/scene/material_normalmap.glsl"
#endif

struct Material
{
	vec4 Base;
	vec4 Bright;
	vec4 Glow;
	vec3 Normal;
	vec3 Specular;
	float Glossiness;
	float SpecularLevel;
	float Metallic;
	float Roughness;
	float AO;
};

vec4 Process(vec4 color);
void SetupMaterial(inout Material mat);
vec3 ProcessMaterialLight(Material material, vec3 color, float sunlightAttenuation);
vec3 ProcessSWLight(Material material, float sunlightAttenuation);
vec2 GetTexCoord();

Material CreateMaterial()
{
	Material material;
	material.Base = vec4(0.0);
	material.Bright = vec4(0.0);
	material.Glow = vec4(0.0);
	material.Normal = vec3(0.0);
	material.Specular = vec3(0.0);
	material.Glossiness = 0.0;
	material.SpecularLevel = 0.0;
	material.Metallic = 0.0;
	material.Roughness = 0.0;
	material.AO = 0.0;
	SetupMaterial(material);
	return material;
}

#ifndef SIMPLE3D
	void SetMaterialProps(inout Material material, vec2 texCoord)
	{
		#ifdef NPOT_EMULATION
			if (uNpotEmulation.y != 0.0)
			{
				float period = floor(texCoord.t / uNpotEmulation.y);
				texCoord.s += uNpotEmulation.x * floor(mod(texCoord.t, uNpotEmulation.y));
				texCoord.t = period + mod(texCoord.t, uNpotEmulation.y);
			}
		#endif	
			material.Base = getTexel(texCoord.st); 
			material.Normal = ApplyNormalMap(texCoord.st);
			
		// OpenGL doesn't care, but Vulkan pukes all over the place if these texture samplings are included in no-texture shaders, even though never called.
		#ifndef NO_LAYERS
			if (TEXF_Brightmap)
			{
				material.Bright = desaturate(texture(brighttexture, texCoord.st));
			}
			
			if (TEXF_Detailmap)
			{
				vec4 Detail = texture(detailtexture, texCoord.st * uDetailParms.xy) * uDetailParms.z;
				material.Base.rgb *= Detail.rgb;
			}
			
			if (TEXF_Glowmap)
			{
				material.Glow = desaturate(texture(glowtexture, texCoord.st));
			}
			
			#ifdef PBR
				material.Metallic = texture(metallictexture, texCoord.st).r;
				material.Roughness = texture(roughnesstexture, texCoord.st).r;
				material.AO = texture(aotexture, texCoord.st).r;
			#endif
			
			#ifdef SPECULAR
				material.Specular = texture(speculartexture, texCoord.st).rgb;
				material.Glossiness = uSpecularMaterial.x;
				material.SpecularLevel = uSpecularMaterial.y;
			#endif
		#endif
	}
#endif