/*
** textures.h
**
**---------------------------------------------------------------------------
** Copyright 2005-2016 Randy Heit
** Copyright 2005-2016 Christoph Oelckers
** All rights reserved.
**
** Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions
** are met:
**
** 1. Redistributions of source code must retain the above copyright
**    notice, this list of conditions and the following disclaimer.
** 2. Redistributions in binary form must reproduce the above copyright
**    notice, this list of conditions and the following disclaimer in the
**    documentation and/or other materials provided with the distribution.
** 3. The name of the author may not be used to endorse or promote products
**    derived from this software without specific prior written permission.
**
** THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
** IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
** OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
** IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
** INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
** NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
** DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
** THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
** (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
** THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
**---------------------------------------------------------------------------
**
*/

#ifndef __TEXTURES_H
#define __TEXTURES_H

#include "basics.h"
#include "vectors.h"
#include "colormatcher.h"
#include "renderstyle.h"
#include "textureid.h"
#include <vector>
#include "hw_texcontainer.h"
#include "refcounted.h"

// 15 because 0th texture is our texture
#define MAX_CUSTOM_HW_SHADER_TEXTURES 15

typedef TMap<int, bool> SpriteHits;
class FImageSource;
class FGameTexture;
class IHardwareTexture;

enum MaterialShaderIndex
{
	SHADER_Default,
	SHADER_Warp1,
	SHADER_Warp2,
	SHADER_Specular,
	SHADER_PBR,
	SHADER_Paletted,
	SHADER_NoTexture,
	SHADER_BasicFuzz,
	SHADER_SmoothFuzz,
	SHADER_SwirlyFuzz,
	SHADER_TranslucentFuzz,
	SHADER_JaggedFuzz,
	SHADER_NoiseFuzz,
	SHADER_SmoothNoiseFuzz,
	SHADER_SoftwareFuzz,
	FIRST_USER_SHADER
};

enum texflags
{
	// These get Or'ed into uTextureMode because it only uses its 3 lowermost bits.
	TEXF_Brightmap = 0x10000,
	TEXF_Detailmap = 0x20000,
	TEXF_Glowmap = 0x40000,
};



enum
{
	SFlag_Brightmap = 1,
	SFlag_Detailmap = 2,
	SFlag_Glowmap = 4,
};

struct UserShaderDesc
{
	FString shader;
	MaterialShaderIndex shaderType;
	FString defines;
	bool disablealphatest = false;
	uint8_t shaderFlags = 0;
};

extern TArray<UserShaderDesc> usershaders;


struct FloatRect
{
	float left,top;
	float width,height;


	void Offset(float xofs,float yofs)
	{
		left+=xofs;
		top+=yofs;
	}
	void Scale(float xfac,float yfac)
	{
		left*=xfac;
		width*=xfac;
		top*=yfac;
		height*=yfac;
	}
};

class FBitmap;
struct FRemapTable;
struct FCopyInfo;
class FScanner;

// Texture IDs
class FTextureManager;
class FTerrainTypeArray;
class IHardwareTexture;
class FMaterial;
class FMultipatchTextureBuilder;

extern int r_spriteadjustSW, r_spriteadjustHW;

class FNullTextureID : public FTextureID
{
public:
	FNullTextureID() : FTextureID(0) {}
};

enum FTextureFormat : uint32_t
{
	TEX_Pal,
	TEX_Gray,
	TEX_RGB,		// Actually ARGB

	TEX_Count
};

class ISoftwareTexture
{
public:
	virtual ~ISoftwareTexture() = default;
};

class FGLRenderState;

struct spriteframewithrotate;
class FSerializer;
namespace OpenGLRenderer
{
	class FGLRenderState;
	class FHardwareTexture;
}

union FContentIdBuilder
{
	uint64_t id;
	struct
	{
		unsigned imageID : 24;
		unsigned translation : 16;
		unsigned expand : 1;
		unsigned scaler : 4;
		unsigned scalefactor : 4;
	};
};

struct FTextureBuffer
{
	uint8_t *mBuffer = nullptr;
	int mWidth = 0;
	int mHeight = 0;
	uint64_t mContentId = 0;	// unique content identifier. (Two images created from the same image source with the same settings will return the same value.)

	FTextureBuffer() = default;

	~FTextureBuffer()
	{
		if (mBuffer) delete[] mBuffer;
	}

	FTextureBuffer(const FTextureBuffer &other) = delete;
	FTextureBuffer(FTextureBuffer &&other)
	{
		mBuffer = other.mBuffer;
		mWidth = other.mWidth;
		mHeight = other.mHeight;
		mContentId = other.mContentId;
		other.mBuffer = nullptr;
	}

	FTextureBuffer& operator=(FTextureBuffer &&other)
	{
		mBuffer = other.mBuffer;
		mWidth = other.mWidth;
		mHeight = other.mHeight;
		mContentId = other.mContentId;
		other.mBuffer = nullptr;
		return *this;
	}

};

struct SpritePositioningInfo
{
	uint16_t trim[4];
	int spriteWidth, spriteHeight;
	float mSpriteU[2], mSpriteV[2];
	FloatRect mSpriteRect;
	uint8_t mTrimResult;

	float GetSpriteUL() const { return mSpriteU[0]; }
	float GetSpriteVT() const { return mSpriteV[0]; }
	float GetSpriteUR() const { return mSpriteU[1]; }
	float GetSpriteVB() const { return mSpriteV[1]; }

	const FloatRect &GetSpriteRect() const
	{
		return mSpriteRect;
	}

};

// Base texture class
class FTexture : public RefCountedBase
{
	friend class FGameTexture;	// only for the porting work
	friend class FTexture;
	friend struct FTexCoordInfo;
	friend class FMultipatchTextureBuilder;
	friend class FMaterial;
	friend class FFont;


public:

	IHardwareTexture* GetHardwareTexture(int translation, int scaleflags);
	static FTexture *CreateTexture(const char *name, int lumpnum, ETextureType UseType);
	virtual ~FTexture ();
	virtual FImageSource *GetImage() const { return nullptr; }
	void CreateUpsampledTextureBuffer(FTextureBuffer &texbuffer, bool hasAlpha, bool checkonly);
	void CleanHardwareTextures(bool reallyclean);
	void SetSpriteRect();

	// These are mainly meant for 2D code which only needs logical information about the texture to position it properly.
	int GetDisplayWidth() { int foo = int((Width * 2) / Scale.X); return (foo >> 1) + (foo & 1); }
	int GetDisplayHeight() { int foo = int((Height * 2) / Scale.Y); return (foo >> 1) + (foo & 1); }
	double GetDisplayWidthDouble() { return Width / Scale.X; }
	double GetDisplayHeightDouble() { return Height / Scale.Y; }
	int GetDisplayLeftOffset() { return GetScaledLeftOffset(0); }
	int GetDisplayTopOffset() { return GetScaledTopOffset(0); }
	double GetDisplayLeftOffsetDouble(int adjusted = 0) { return _LeftOffset[adjusted] / Scale.X; }
	double GetDisplayTopOffsetDouble(int adjusted = 0) { return _TopOffset[adjusted] / Scale.Y; }

	int GetTexelWidth() { return Width; }
	int GetTexelHeight() { return Height; }
	int GetTexelLeftOffset(int adjusted) { return _LeftOffset[adjusted]; }
	int GetTexelTopOffset(int adjusted) { return _TopOffset[adjusted]; }

	
	bool isSkybox() const { return bSkybox; }
	bool isFullbrightDisabled() const { return bDisableFullbright; }
	bool isHardwareCanvas() const { return bHasCanvas; }	// There's two here so that this can deal with software canvases in the hardware renderer later.
	bool isCanvas() const { return bHasCanvas; }
	int isWarped() const { return bWarped; }
	int GetRotations() const { return Rotations; }
	float GetShaderSpeed() const { return shaderspeed; }
	void SetRotations(int rot) { Rotations = int16_t(rot); }
	
	const FString &GetName() const { return Name; }
	void SetNoDecals(bool on) { bNoDecals = on;  }
	void SetWarpStyle(int style) { bWarped = style; }
	bool allowNoDecals() const { return bNoDecals; }
	bool isScaled() const { return Scale.X != 1 || Scale.Y != 1; }
	bool isMasked() const { return bMasked; }
	void SetSkyOffset(int offs) { SkyOffset = offs; }
	int GetSkyOffset() const { return SkyOffset; }
	FTextureID GetID() const { return id; }
	PalEntry GetSkyCapColor(bool bottom);
	virtual int GetSourceLump() { return SourceLump; }	// needed by the scripted GetName method.
	void GetGlowColor(float *data);
	bool isGlowing() const { return bGlowing; }
	bool isAutoGlowing() const { return bAutoGlowing; }
	int GetGlowHeight() const { return GlowHeight; }
	bool isFullbright() const { return bFullbright; }
	bool FindHoles(const unsigned char * buffer, int w, int h);
	int GetSourceLump() const { return SourceLump;  }
	void SetSpeed(float fac) { shaderspeed = fac; }
	bool UseWorldPanning() const  { return bWorldPanning; }
	void SetWorldPanning(bool on) { bWorldPanning = on; }
	void SetDisplaySize(int fitwidth, int fitheight);


	void CopySize(FTexture* BaseTexture)
	{
		Width = BaseTexture->GetTexelWidth();
		Height = BaseTexture->GetTexelHeight();
		_TopOffset[0] = BaseTexture->_TopOffset[0];
		_TopOffset[1] = BaseTexture->_TopOffset[1];
		_LeftOffset[0] = BaseTexture->_LeftOffset[0];
		_LeftOffset[1] = BaseTexture->_LeftOffset[1];
		Scale = BaseTexture->Scale;
	}

	// This is only used for the null texture and for Heretic's skies.
	void SetSize(int w, int h)
	{
		Width = w;
		Height = h;
	}

	bool TrimBorders(uint16_t* rect);
	int GetAreas(FloatRect** pAreas) const;

	// Returns the whole texture, stored in column-major order
	virtual TArray<uint8_t> Get8BitPixels(bool alphatex);
	virtual FBitmap GetBgraBitmap(const PalEntry *remap, int *trans = nullptr);

	static bool SmoothEdges(unsigned char * buffer,int w, int h);
	static PalEntry averageColor(const uint32_t *data, int size, int maxout);

protected:

	DVector2 Scale;

	int SourceLump;
	FTextureID id;

public:
	FHardwareTextureContainer SystemTextures;
protected:

	protected:

	FString Name;

	uint8_t bNoDecals:1;		// Decals should not stick to texture
	uint8_t bNoRemap0:1;		// Do not remap color 0 (used by front layer of parallax skies)
	uint8_t bWorldPanning:1;	// Texture is panned in world units rather than texels
	uint8_t bMasked:1;			// Texture (might) have holes
	uint8_t bAlphaTexture:1;	// Texture is an alpha channel without color information
	uint8_t bHasCanvas:1;		// Texture is based off FCanvasTexture
	uint8_t bWarped:2;			// This is a warped texture. Used to avoid multiple warps on one texture
	uint8_t bComplex:1;		// Will be used to mark extended MultipatchTextures that have to be
							// fully composited before subjected to any kind of postprocessing instead of
							// doing it per patch.
	uint8_t bMultiPatch:2;		// This is a multipatch texture (we really could use real type info for textures...)
	uint8_t bFullNameTexture : 1;
	uint8_t bBrightmapChecked : 1;				// Set to 1 if brightmap has been checked
	public:
	uint8_t bGlowing : 1;						// Texture glow color
	uint8_t bAutoGlowing : 1;					// Glow info is determined from texture image.
	uint8_t bFullbright : 1;					// always draw fullbright
	uint8_t bDisableFullbright : 1;				// This texture will not be displayed as fullbright sprite
	protected:
	uint8_t bSkybox : 1;						// is a cubic skybox
	uint8_t bNoCompress : 1;
	int8_t bTranslucent : 2;
	int8_t bExpandSprite = -1;
	bool bHiresHasColorKey = false;				// Support for old color-keyed Doomsday textures

	uint16_t Rotations;
	int16_t SkyOffset;
	FloatRect *areas = nullptr;
	int areacount = 0;
	public:
	int GlowHeight = 128;
	PalEntry GlowColor = 0;
	private:
	float Glossiness = 10.f;
	float SpecularLevel = 0.1f;
	float shaderspeed = 1.f;
	int shaderindex = 0;

	int GetScaledWidth () { int foo = int((Width * 2) / Scale.X); return (foo >> 1) + (foo & 1); }
	int GetScaledHeight () { int foo = int((Height * 2) / Scale.Y); return (foo >> 1) + (foo & 1); }
	double GetScaledWidthDouble () { return Width / Scale.X; }
	double GetScaledHeightDouble () { return Height / Scale.Y; }
	double GetScaleY() const { return Scale.Y; }

	// Now with improved offset adjustment.
	int GetLeftOffset(int adjusted) { return _LeftOffset[adjusted]; }
	int GetTopOffset(int adjusted) { return _TopOffset[adjusted]; }
	int GetScaledLeftOffset (int adjusted) { int foo = int((_LeftOffset[adjusted] * 2) / Scale.X); return (foo >> 1) + (foo & 1); }
	int GetScaledTopOffset (int adjusted) { int foo = int((_TopOffset[adjusted] * 2) / Scale.Y); return (foo >> 1) + (foo & 1); }

	// Interfaces for the different renderers. Everything that needs to check renderer-dependent offsets
	// should use these, so that if changes are needed, this is the only place to edit.

	// For the hardware renderer. The software renderer's have been offloaded to FSoftwareTexture
	int GetLeftOffsetHW() { return _LeftOffset[r_spriteadjustHW]; }
	int GetTopOffsetHW() { return _TopOffset[r_spriteadjustHW]; }

	virtual void ResolvePatches() {}

public:
	void SetScale(const DVector2 &scale)
	{
		Scale = scale;
	}

protected:
	uint16_t Width, Height;
	int16_t _LeftOffset[2], _TopOffset[2];

	FTexture (const char *name = NULL, int lumpnum = -1);

public:
	FTextureBuffer CreateTexBuffer(int translation, int flags = 0);
	virtual bool DetermineTranslucency();
	bool GetTranslucency()
	{
		return bTranslucent != -1 ? bTranslucent : DetermineTranslucency();
	}

private:
	int CheckDDPK3();
	int CheckExternalFile(bool & hascolorkey);
	
	bool bSWSkyColorDone = false;
	PalEntry FloorSkyColor;
	PalEntry CeilingSkyColor;

public:

	void CheckTrans(unsigned char * buffer, int size, int trans);
	bool ProcessData(unsigned char * buffer, int w, int h, bool ispatch);
	int CheckRealHeight();

	friend class FTextureManager;
};


// A texture that can be drawn to.

class FCanvasTexture : public FTexture
{
public:
	FCanvasTexture(const char* name, int width, int height)
	{
		Name = name;
		Width = width;
		Height = height;

		bMasked = false;
		bHasCanvas = true;
		bTranslucent = false;
		bExpandSprite = false;
	}

	void NeedUpdate() { bNeedsUpdate = true; }
	void SetUpdated(bool rendertype) { bNeedsUpdate = false; bFirstUpdate = false; bLastUpdateType = rendertype; }

protected:

	bool bLastUpdateType = false;
	bool bNeedsUpdate = true;
public:
	bool bFirstUpdate = true;

	friend struct FCanvasTextureInfo;
};


// A wrapper around a hardware texture, to allow using it in the 2D drawing interface.
class FWrapperTexture : public FTexture
{
	int Format;
public:
	FWrapperTexture(int w, int h, int bits = 1);
	IHardwareTexture *GetSystemTexture()
	{
		return SystemTextures.GetHardwareTexture(0, 0);
	}

	int GetColorFormat() const
	{
		return Format;
	}
};


class FImageTexture : public FTexture
{
	FImageSource* mImage;
protected:
	FImageTexture(const char *name) : FTexture(name) {}
	void SetFromImage();
public:
	FImageTexture(FImageSource* image, const char* name = nullptr) noexcept;
	virtual TArray<uint8_t> Get8BitPixels(bool alphatex);

	void SetImage(FImageSource* img)	// This is only for the multipatch texture builder!
	{
		mImage = img;
	}

	FImageSource* GetImage() const override { return mImage; }
	FBitmap GetBgraBitmap(const PalEntry* p, int* trans) override;
	bool DetermineTranslucency() override;

};

struct MaterialLayers
{
	float Glossiness;
	float SpecularLevel;
	FGameTexture* Brightmap;
	FGameTexture* Normal;
	FGameTexture* Specular;
	FGameTexture* Metallic;
	FGameTexture* Roughness;
	FGameTexture* AmbientOcclusion;
	FGameTexture* CustomShaderTextures[MAX_CUSTOM_HW_SHADER_TEXTURES];
};

struct FTexCoordInfo
{
	int mRenderWidth;
	int mRenderHeight;
	int mWidth;
	FVector2 mScale;
	FVector2 mTempScale;
	bool mWorldPanning;

	float FloatToTexU(float v) const { return v / mRenderWidth; }
	float FloatToTexV(float v) const { return v / mRenderHeight; }
	float RowOffset(float ofs) const;
	float TextureOffset(float ofs) const;
	float TextureAdjustWidth() const;
	void GetFromTexture(FTexture *tex, float x, float y, bool forceworldpanning);
	void GetFromTexture(FGameTexture* tex, float x, float y, bool forceworldpanning);
};

enum
{
	CLAMP_NONE = 0,
	CLAMP_X = 1,
	CLAMP_Y = 2,
	CLAMP_XY = 3,
	CLAMP_XY_NOMIP = 4,
	CLAMP_NOFILTER = 5,
	CLAMP_CAMTEX = 6,
};


//-----------------------------------------------------------------------------
//
// Todo: Get rid of this
// The faces can easily be stored in the material layer array
//
//-----------------------------------------------------------------------------

class FSkyBox : public FImageTexture
{
public:

	FGameTexture* previous;
	FGameTexture* faces[6];	// the faces need to be full materials as they can have all supported effects.
	bool fliptop;

	FSkyBox(const char* name);
	void SetSize();

	bool Is3Face() const
	{
		return faces[5] == nullptr;
	}

	bool IsFlipped() const
	{
		return fliptop;
	}
};

// Refactoring helper to allow piece by piece adjustment of the API
class FGameTexture
{
	friend class FMaterial;

	// Material layers. These are shared so reference counting is used.
	RefCountedPtr<FTexture> Base;
	RefCountedPtr<FTexture> Brightmap;
	RefCountedPtr<FTexture> Detailmap;
	RefCountedPtr<FTexture> Glowmap;
	RefCountedPtr<FTexture> Normal;							// Normal map texture
	RefCountedPtr<FTexture> Specular;						// Specular light texture for the diffuse+normal+specular light model
	RefCountedPtr<FTexture> Metallic;						// Metalness texture for the physically based rendering (PBR) light model
	RefCountedPtr<FTexture> Roughness;						// Roughness texture for PBR
	RefCountedPtr<FTexture> AmbientOcclusion;				// Ambient occlusion texture for PBR
	RefCountedPtr<FTexture> CustomShaderTextures[MAX_CUSTOM_HW_SHADER_TEXTURES]; // Custom texture maps for custom hardware shaders

	int8_t shouldUpscaleFlag = 0;				// Without explicit setup, scaling is disabled for a texture.
	ETextureType UseType = ETextureType::Wall;	// This texture's primary purpose
	SpritePositioningInfo* spi = nullptr;

	ISoftwareTexture* SoftwareTexture = nullptr;
	FMaterial* Material[4] = {  };

public:
	FGameTexture(FTexture* wrap) : Base(wrap) {}
	~FGameTexture();
	void CreateDefaultBrightmap();
	void AddAutoMaterials();
	bool ShouldExpandSprite();
	void SetupSpriteData();
	void SetSpriteRect();

	ETextureType GetUseType() const { return UseType; }
	void SetUpscaleFlag(int what) { shouldUpscaleFlag = what; }
	int GetUpscaleFlag() { return shouldUpscaleFlag; }

	FTexture* GetTexture() { return Base.get(); }
	int GetSourceLump() const { return Base->GetSourceLump(); }
	void SetBrightmap(FGameTexture* tex) { Brightmap = tex->GetTexture(); }

	double GetDisplayWidth() /*const*/ { return Base->GetDisplayWidthDouble(); }
	double GetDisplayHeight() /*const*/ { return Base->GetDisplayHeightDouble(); }
	int GetTexelWidth() /*const*/ { return Base->GetTexelWidth(); }
	int GetTexelHeight() /*const*/ { return Base->GetTexelHeight(); }
	int GetTexelLeftOffset(int adjusted = 0) /*const*/ { return Base->GetTexelLeftOffset(adjusted); }
	int GetTexelTopOffset(int adjusted = 0) /*const*/ { return Base->GetTexelTopOffset(adjusted); }
	double GetDisplayLeftOffset(int adjusted = 0) /*const*/ { return Base->GetDisplayLeftOffsetDouble(adjusted); }
	double GetDisplayTopOffset(int adjusted = 0) /*const*/ { return Base->GetDisplayTopOffsetDouble(adjusted); }
	// For the hardware renderer. The software renderer's have been offloaded to FSoftwareTexture
	int GetLeftOffsetHW() { return GetTexelLeftOffset(r_spriteadjustHW); }
	int GetTopOffsetHW() { return GetTexelTopOffset(r_spriteadjustHW); }

	bool isValid() const { return UseType != ETextureType::Null; }
	int isWarped() { return Base->isWarped(); }
	void SetWarpStyle(int style) { Base->bWarped = style; }
	bool isMasked() { return Base->isMasked(); }
	bool isHardwareCanvas() const { return Base->isHardwareCanvas(); }	// There's two here so that this can deal with software canvases in the hardware renderer later.
	bool isSoftwareCanvas() const { return Base->isCanvas(); }
	bool isMiscPatch() const { return GetUseType() == ETextureType::MiscPatch; }	// only used by the intermission screen to decide whether to tile the background image or not. 
	bool isMultiPatch() const { return Base->bMultiPatch; }
	bool isFullbrightDisabled() const { return Base->isFullbrightDisabled(); }
	bool isFullbright() const { return Base->isFullbright(); }
	bool isFullNameTexture() const { return Base->bFullNameTexture; }
	bool expandSprites() const { return Base->bExpandSprite; }
	bool useWorldPanning() const { return Base->UseWorldPanning();  }
	void SetWorldPanning(bool on) { Base->SetWorldPanning(on); }
	bool allowNoDecals() const { return Base->allowNoDecals(); }
	void SetNoDecals(bool on) { Base->bNoDecals = on; }
	void SetTranslucent(bool on) { Base->bTranslucent = on; }
	void SetUseType(ETextureType type) { UseType = type; }
	int GetShaderIndex() const { return Base->shaderindex; }
	float GetShaderSpeed() const { return Base->GetShaderSpeed(); }
	uint16_t GetRotations() const { return Base->GetRotations(); }
	void SetRotations(int index) { Base->SetRotations(index); }
	void SetSkyOffset(int ofs) { Base->SetSkyOffset(ofs); }
	int GetSkyOffset() const { return Base->GetSkyOffset(); }
	FTextureID GetID() const { return Base->GetID(); }
	void SetScale(DVector2 vec) { Base->SetScale(vec); }

	ISoftwareTexture* GetSoftwareTexture()
	{
		return SoftwareTexture;
	}
	void SetSoftwareTexture(ISoftwareTexture* swtex)
	{
		SoftwareTexture = swtex;
	}

	FMaterial* GetMaterial(int num)
	{
		return Material[num];
	}

	const FString& GetName() const { return Base->GetName(); }
	void SetShaderSpeed(float speed) { Base->shaderspeed = speed; }
	void SetShaderIndex(int index) { Base->shaderindex = index; }
	void SetShaderLayers(MaterialLayers& lay)
	{
		// Only update layers that have something defind.
		if (lay.Glossiness > -1000) Base->Glossiness = lay.Glossiness;
		if (lay.SpecularLevel > -1000) Base->SpecularLevel = lay.SpecularLevel;
		if (lay.Brightmap) Brightmap = lay.Brightmap->GetTexture();
		if (lay.Normal) Normal = lay.Normal->GetTexture();
		if (lay.Specular) Specular = lay.Specular->GetTexture();
		if (lay.Metallic) Metallic = lay.Metallic->GetTexture();
		if (lay.Roughness) Roughness = lay.Roughness->GetTexture();
		if (lay.AmbientOcclusion) AmbientOcclusion = lay.AmbientOcclusion->GetTexture();
		for (int i = 0; i < MAX_CUSTOM_HW_SHADER_TEXTURES; i++)
		{
			if (lay.CustomShaderTextures[i]) CustomShaderTextures[i] = lay.CustomShaderTextures[i]->GetTexture();
		}
	}
	float GetGlossiness() const { return Base->Glossiness; }
	float GetSpecularLevel() const { return Base->SpecularLevel; }

	void CopySize(FGameTexture* BaseTexture)
	{
		Base->CopySize(BaseTexture->Base.get());
	}

	// Glowing is a pure material property that should not filter down to the actual texture objects.
	void GetGlowColor(float* data) { Base->GetGlowColor(data); }
	bool isGlowing() const { return Base->isGlowing(); }
	bool isAutoGlowing() const { return Base->isAutoGlowing(); }
	int GetGlowHeight() const { return Base->GetGlowHeight(); }
	void SetAutoGlowing() { auto tex = GetTexture(); tex->bAutoGlowing = tex->bGlowing = tex->bFullbright = true; }
	void SetGlowHeight(int v) { Base->GlowHeight = v; }
	void SetFullbright() { Base->bFullbright = true;  }
	void SetDisableFullbright(bool on) { Base->bDisableFullbright = on; }
	void SetGlowing(PalEntry color) { auto tex = GetTexture(); tex->bAutoGlowing = false;	tex->bGlowing = true; tex->GlowColor = color; }

	bool isUserContent() const;
	int CheckRealHeight() { return Base->CheckRealHeight(); }
	bool isSkybox() const { return Base->isSkybox(); }
	void SetSize(int x, int y) { Base->SetSize(x, y); }
	void SetDisplaySize(float w, float h) { Base->SetSize((int)w, (int)h); }

	const SpritePositioningInfo& GetSpritePositioning(int which) { if (spi == nullptr) SetupSpriteData(); return spi[which]; }
	int GetAreas(FloatRect** pAreas) const { return Base->GetAreas(pAreas); }
	PalEntry GetSkyCapColor(bool bottom) { return Base->GetSkyCapColor(bottom); }

	bool GetTranslucency()
	{
		return Base->GetTranslucency();
	}

	// Since these properties will later piggyback on existing members of FGameTexture, the accessors need to be here. 
	FGameTexture *GetSkyFace(int num)
	{
		return (isSkybox() ? static_cast<FSkyBox*>(Base.get())->faces[num] : nullptr);
	}
	bool GetSkyFlip() { return isSkybox() ? static_cast<FSkyBox*>(Base.get())->fliptop : false; }

	int GetClampMode(int clampmode)
	{
		if (GetUseType() == ETextureType::SWCanvas) clampmode = CLAMP_NOFILTER;
		else if (isHardwareCanvas()) clampmode = CLAMP_CAMTEX;
		else if ((isWarped() || Base->shaderindex >= FIRST_USER_SHADER) && clampmode <= CLAMP_XY) clampmode = CLAMP_NONE;
		return clampmode;
	}
};

inline FGameTexture* MakeGameTexture(FTexture* tex, ETextureType useType)
{
	if (!tex) return nullptr;
	auto t = new FGameTexture(tex);
	t->SetUseType(useType);
	return t;
}

enum EUpscaleFlags
{
	UF_None = 0,
	UF_Texture = 1,
	UF_Sprite = 2,
	UF_Font = 4
};

extern int upscalemask;
void UpdateUpscaleMask();

int calcShouldUpscale(FGameTexture* tex);
inline int shouldUpscale(FGameTexture* tex, EUpscaleFlags UseType)
{
	// This only checks the global scale mask and the texture's validation for upscaling. Everything else has been done up front elsewhere.
	if (!(upscalemask & UseType)) return 0;
	return tex->GetUpscaleFlag();
}

#endif

