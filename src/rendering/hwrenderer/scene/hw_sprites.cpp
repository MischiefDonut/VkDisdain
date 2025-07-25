// 
//---------------------------------------------------------------------------
//
// Copyright(C) 2002-2016 Christoph Oelckers
// All rights reserved.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this program.  If not, see http://www.gnu.org/licenses/
//
//--------------------------------------------------------------------------
//
/*
** gl_sprite.cpp
** Sprite/Particle rendering
**
*/

#include "p_local.h"
#include "p_effect.h"
#include "g_level.h"
#include "doomstat.h"
#include "r_defs.h"
#include "r_sky.h"
#include "r_utility.h"
#include "a_pickups.h"
#include "d_player.h"
#include "g_levellocals.h"
#include "events.h"
#include "actorinlines.h"
#include "r_data/r_vanillatrans.h"
#include "matrix.h"
#include "models.h"
#include "vectors.h"
#include "texturemanager.h"
#include "basics.h"

#include "hw_models.h"
#include "hwrenderer/scene/hw_drawstructs.h"
#include "hwrenderer/scene/hw_drawinfo.h"
#include "hwrenderer/scene/hw_fakeflat.h"
#include "hwrenderer/scene/hw_portal.h"
#include "flatvertices.h"
#include "hw_cvars.h"
#include "hw_clock.h"
#include "hw_lighting.h"
#include "hw_material.h"
#include "hw_dynlightdata.h"
#include "hw_renderstate.h"
#include "hw_drawcontext.h"
#include "quaternion.h"

#include "p_visualthinker.h"

extern TArray<spritedef_t> sprites;
extern TArray<spriteframe_t> SpriteFrames;
extern uint32_t r_renderercaps;
extern TArray<AActor*> Coronas;

const float LARGE_VALUE = 1e19f;
const float MY_SQRT2    = 1.41421356237309504880; // sqrt(2)

EXTERN_CVAR(Bool, r_debug_disable_vis_filter)
EXTERN_CVAR(Float, transsouls)
EXTERN_CVAR(Float, r_actorspriteshadowalpha)
EXTERN_CVAR(Float, r_actorspriteshadowfadeheight)
EXTERN_CVAR(Bool, gl_aalines)

//==========================================================================
//
// Sprite CVARs
//
//==========================================================================

CVAR(Bool, gl_usecolorblending, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, gl_sprite_blend, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);
CVAR(Int, gl_spriteclip, 2, CVAR_ARCHIVE)
CVAR(Bool, r_debug_nolimitanamorphoses, false, 0)
CVAR(Float, r_spriteclipanamorphicminbias, 0.6, CVAR_ARCHIVE)
CVAR(Float, gl_sclipthreshold, 10.0, CVAR_ARCHIVE)
CVAR(Float, gl_sclipfactor, 1.8f, CVAR_ARCHIVE)
CVAR(Int, gl_particles_style, 1, CVAR_ARCHIVE | CVAR_GLOBALCONFIG) // 0 = square, 1 = round, 2 = smooth
CVAR(Int, gl_billboard_mode, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, gl_billboard_faces_camera, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, hw_force_cambbpref, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, gl_billboard_particles, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CUSTOM_CVAR(Int, gl_fuzztype, 8, CVAR_ARCHIVE)
{
	if (self < 0 || self > 8) self = 0;
}

// [Nash]
CVARD(Bool, r_showhitbox, false, CVAR_GLOBALCONFIG | CVAR_CHEAT, "show actor hitboxes");

//==========================================================================
//
// 
//
//==========================================================================

void HWSprite::DrawSprite(HWDrawInfo *di, FRenderState &state, bool translucent)
{
	int gl_spritelight = get_gl_spritelight();

	// [Disdain] Need proper lighting for VisualThinker models
	if(!actor && !(spr && spr->modelClass) && gl_spritelight > 1) gl_spritelight = 1;

	state.SetShadeVertex(gl_spritelight == 1);
	bool additivefog = false;
	bool foglayer = false;
	int rel = fullbright ? 0 : getExtraLight();
	auto &vp = di->Viewpoint;	

	state.SetLightProbeIndex(actor && actor->Sector ? actor->Sector->lightProbe.index : 0);

	if (translucent)
	{
		// The translucent pass requires special setup for the various modes.

		// for special render styles brightmaps would not look good - especially for subtractive.
		if (RenderStyle.BlendOp != STYLEOP_Add)
		{
			state.EnableBrightmap(false);
		}

		// Optionally use STYLE_ColorBlend in place of STYLE_Add for fullbright items.
		if (RenderStyle == LegacyRenderStyles[STYLE_Add] && trans > 1.f - FLT_EPSILON &&
			gl_usecolorblending && !di->isFullbrightScene() && actor &&
			fullbright && texture && !texture->GetTranslucency())
		{
			RenderStyle = LegacyRenderStyles[STYLE_ColorAdd];
		}

		state.SetRenderStyle(RenderStyle);
		state.SetTextureMode(RenderStyle);

		if (hw_styleflags == STYLEHW_NoAlphaTest)
		{
			state.AlphaFunc(Alpha_GEqual, 0.f);
		}
		else if (!texture || !texture->GetTranslucency()) state.AlphaFunc(Alpha_GEqual, gl_mask_sprite_threshold);
		else state.AlphaFunc(Alpha_Greater, 0.f);

		if (RenderStyle.BlendOp == STYLEOP_Shadow)
		{
			float fuzzalpha = 0.44f;
			float minalpha = 0.1f;

			// fog + fuzz don't work well without some fiddling with the alpha value!
			if (!Colormap.FadeColor.isBlack())
			{
				float dist = Dist2(vp.Pos.X, vp.Pos.Y, x, y);
				int fogd = GetFogDensity(di->Level, di->lightmode, lightlevel, Colormap.FadeColor, Colormap.FogDensity, Colormap.BlendFactor);

				// this value was determined by trial and error and is scale dependent!
				float factor = 0.05f + exp(-fogd * dist / 62500.f);
				fuzzalpha *= factor;
				minalpha *= factor;
			}

			state.AlphaFunc(Alpha_GEqual, gl_mask_sprite_threshold);
			state.SetColor(0.2f, 0.2f, 0.2f, fuzzalpha, Colormap.Desaturation);
			additivefog = true;
			lightlist = nullptr;	// the fuzz effect does not use the sector's light di->Level-> so splitting is not needed.
		}
		else if (RenderStyle.BlendOp == STYLEOP_Add && RenderStyle.DestAlpha == STYLEALPHA_One)
		{
			additivefog = true;
		}
	}
	else if (modelframe == nullptr)
	{
		// This still needs to set the texture mode. As blend mode it will always use GL_ONE/GL_ZERO
		state.SetTextureMode(RenderStyle);
		state.SetDepthBias(-1, -128);
	}
	if (RenderStyle.BlendOp != STYLEOP_Shadow)
	{
		if (di->Level->HasDynamicLights && !di->isFullbrightScene() && !fullbright)
		{
			if (dynlightindex == -1)	// only set if we got no light buffer index. This covers all cases where sprite lighting is used.
			{
				float out[3] = {};
				di->GetDynSpriteLight(gl_light_sprites ? actor : nullptr, gl_light_particles ? particle : nullptr, (gl_light_particles && spr != nullptr) ? &spr->StaticLightsTraceCache : nullptr, out);
				state.SetDynLight(out[0], out[1], out[2]);
			}
		}
		sector_t *cursec = actor ? actor->Sector : particle ? particle->subsector->sector : nullptr;
		if (cursec != nullptr)
		{
			const PalEntry finalcol = fullbright
				? ThingColor
				: ThingColor.Modulate(cursec->SpecialColors[sector_t::sprites]);

			state.SetObjectColor(finalcol);
			state.SetAddColor(cursec->AdditiveColors[sector_t::sprites] | 0xff000000);
		}
		SetColor(state, di->Level, di->lightmode, lightlevel, rel, di->isFullbrightScene(), Colormap, trans);
	}


	if (Colormap.FadeColor.isBlack()) foglevel = lightlevel;

	if (RenderStyle.Flags & STYLEF_FadeToBlack)
	{
		Colormap.FadeColor = 0;
		additivefog = true;
	}

	if (RenderStyle.BlendOp == STYLEOP_RevSub || RenderStyle.BlendOp == STYLEOP_Sub)
	{
		if (!modelframe)
		{
			// non-black fog with subtractive style needs special treatment
			if (!Colormap.FadeColor.isBlack())
			{
				foglayer = true;
				// Due to the two-layer approach we need to force an alpha test that lets everything pass
				state.AlphaFunc(Alpha_Greater, 0);
			}
		}
		else RenderStyle.BlendOp = STYLEOP_Fuzz;	// subtractive with models is not going to work.
	}

	if (!foglayer) SetFog(state, di->Level, di->lightmode, foglevel, rel, di->isFullbrightScene(), &Colormap, additivefog, di->drawctx->portalState.inskybox);
	else
	{
		state.EnableFog(false);
		state.SetFog(0, 0);
	}

	int clampmode = CLAMP_XY;

	if (texture && texture->isNoMipmap())
	{
		clampmode = CLAMP_XY_NOMIP;
	}

	uint32_t spritetype = actor? uint32_t(actor->renderflags & RF_SPRITETYPEMASK) : 0;
	if (texture) state.SetMaterial(texture, UF_Sprite, (spritetype == RF_FACESPRITE) ? CTF_Expand : 0, clampmode, translation, OverrideShader, actor ? actor->GetClass() : nullptr);
	else if (!modelframe) state.EnableTexture(false);

	if (actor && texture)
	{
		int binding = state.getShaderIndex();

		if(binding >= FIRST_USER_SHADER)
		{
			usershaders[binding - FIRST_USER_SHADER].BindActorFields(actor);
		}
	}

	//SetColor(lightlevel, rel, Colormap, trans);

	unsigned int iter = lightlist ? lightlist->Size() : 1;
	bool clipping = false;
	if (lightlist || topclip != LARGE_VALUE || bottomclip != -LARGE_VALUE)
	{
		clipping = true;
		state.EnableSplit(true);
	}

	secplane_t bottomp = { { 0, 0, -1. }, bottomclip, 1. };
	secplane_t topp = { { 0, 0, -1. }, topclip, 1. };
	for (unsigned i = 0; i < iter; i++)
	{
		if (lightlist)
		{
			// set up the light slice
			secplane_t *topplane = i == 0 ? &topp : &(*lightlist)[i].plane;
			secplane_t *lowplane = i == (*lightlist).Size() - 1 ? &bottomp : &(*lightlist)[i + 1].plane;
			int thislight = (*lightlist)[i].caster != nullptr ? hw_ClampLight(*(*lightlist)[i].p_lightlevel) : lightlevel;
			int thisll = actor == nullptr ? thislight : (uint8_t)actor->Sector->CheckSpriteGlow(thislight, actor->InterpolatedPosition(vp.TicFrac));

			FColormap thiscm;
			thiscm.CopyFog(Colormap);
			CopyFrom3DLight(thiscm, &(*lightlist)[i]);
			if (di->Level->flags3 & LEVEL3_NOCOLOREDSPRITELIGHTING)
			{
				thiscm.Decolorize();
			}

			SetColor(state, di->Level, di->lightmode, thisll, rel, di->isFullbrightScene(), thiscm, trans);
			if (!foglayer)
			{
				SetFog(state, di->Level, di->lightmode, thislight, rel, di->isFullbrightScene(), &thiscm, additivefog, di->drawctx->portalState.inskybox);
			}
			SetSplitPlanes(state, *topplane, *lowplane);
		}
		else if (clipping)
		{
			SetSplitPlanes(state, topp, bottomp);
		}

		if(actor)
		{
			state.SetActorCenter(actor->X(), actor->Center(), actor->Y());
		}

		if (!modelframe)
		{
			state.SetNormal(0, 0, 0);

			if(gl_spritelight > 0)
			{
				state.SetLightNoNormals(true);
				if(actor && gl_spritelight < 2)
				{
					state.SetUseSpriteCenter(true);
				}
			}

			CreateVertices(di, state);

			if (polyoffset)
			{
				state.SetDepthBias(-1, -128);
			}

			state.SetLightIndex(dynlightindex);
			state.Draw(DT_TriangleStrip, vertexindex, 4);
			state.SetLightIndex(-1);

			if (foglayer)
			{
				// If we get here we know that we have colored fog and no fixed colormap.
				SetFog(state, di->Level, di->lightmode, foglevel, rel, false, &Colormap, additivefog, di->drawctx->portalState.inskybox);
				state.SetTextureMode(TM_FOGLAYER);
				state.SetRenderStyle(STYLE_Translucent);
				state.Draw(DT_TriangleStrip, vertexindex, 4);
				state.SetTextureMode(TM_NORMAL);
			}
			state.SetLightNoNormals(false);
			state.SetUseSpriteCenter(false);
		}
		else
		{
			FHWModelRenderer renderer(di, state, dynlightindex);
			RenderModel(&renderer, x, y, z, modelframe, actor, this, di->Viewpoint.TicFrac);
			state.SetFlatVertexBuffer();
			state.SetLightIndex(-1);
		}
	}

	if (clipping)
	{
		state.EnableSplit(false);
	}

	if (translucent)
	{
		state.EnableBrightmap(true);
		state.SetRenderStyle(STYLE_Translucent);
		state.SetTextureMode(TM_NORMAL);
		if (actor != nullptr && (actor->renderflags & RF_SPRITETYPEMASK) == RF_FLATSPRITE)
		{
			state.ClearDepthBias();
		}
	}
	else if (modelframe == nullptr)
	{
		state.ClearDepthBias();
	}

	// [Nash] hitbox debug
	if (actor && r_showhitbox)
	{
		additivefog = false;
		state.EnableLineSmooth(true);

		PalEntry hitboxColor = 0x640064;

		if (actor->flags & MF_SOLID)
			hitboxColor = 0x00ff00;
		if (actor->flags & MF_SHOOTABLE)
			hitboxColor = 0xffff00;
		if (actor->flags3 & MF3_ISMONSTER && actor->health > 0)
			hitboxColor = 0xff0000;
		if (actor->flags & MF_SPECIAL && actor->health > 0)
			hitboxColor = 0x00ffff;
		if (actor->flags & MF_NOBLOCKMAP)
			hitboxColor = 0x646464;

		hitboxColor.a = 255;
		state.SetObjectColor(hitboxColor);

		state.SetAddColor(0);
		state.SetDynLight(0, 0, 0);
		state.SetRenderStyle(STYLE_Normal);
		state.SetTextureMode(TM_NORMAL);
		SetFog(state, di->Level, di->lightmode, 0, 0, false, &Colormap, true, di->drawctx->portalState.inskybox);
		SetColor(state, di->Level, di->lightmode, 255, 0, true, Colormap, true);
		state.EnableTexture(false);

		int scales[12][6] =
		{
			// bottom
			{-1, 0, -1, -1, 0, 1}, {-1, 0, 1, 1, 0, 1}, {1, 0, 1, 1, 0, -1}, {1, 0, -1, -1, 0, -1},
			// top
			{-1, 1, -1, -1, 1, 1}, {-1, 1, 1, 1, 1, 1}, {1, 1, 1, 1, 1, -1}, {1, 1, -1, -1, 1, -1},
			// vertical
			{-1, 0, -1, -1, 1, -1}, {-1, 0, 1, -1, 1, 1}, {1, 0, 1, 1, 1, 1}, {1, 0, -1, 1, 1, -1}
			/*
			// projectilepassheight
			{-1, 0, -1, 1, 0, 1}, {-1, 0, 1, 1, 0, -1}
			*/
		};

		for (int i = 0; i < 12; i++)
		{
			auto vert = state.AllocVertices(2);
			auto vp = vert.first;
			unsigned int vertexindex = vert.second;
			vp[0].Set(actor->X() + actor->radius * scales[i][0], actor->Z() + actor->Height * scales[i][1], actor->Y() + actor->radius * scales[i][2], 0.0f, 0.0f);
			vp[1].Set(actor->X() + actor->radius * scales[i][3], actor->Z() + actor->Height * scales[i][4], actor->Y() + actor->radius * scales[i][5], 0.0f, 0.0f);
			state.Draw(DT_Lines, vertexindex, 2);
		}

		state.EnableTexture(true);
	}

	state.SetObjectColor(0xffffffff);
	state.SetAddColor(0);
	state.EnableTexture(true);
	state.SetDynLight(0, 0, 0);
	state.SetShadeVertex(false);
}

//==========================================================================
//
// 
//
//==========================================================================

void HandleSpriteOffsets(Matrix3x4 *mat, const FRotator *HW, FVector2 *offset, bool XYBillboard)
{
	FAngle zero = FAngle::fromDeg(0);
	FAngle pitch = (XYBillboard) ? HW->Pitch : zero;
	FAngle yaw = FAngle::fromDeg(270.) - HW->Yaw;

	FQuaternion quat = FQuaternion::FromAngles(yaw, pitch, zero);
	FVector3 sideVec = quat * FVector3(0, 1, 0);
	FVector3 upVec = quat * FVector3(0, 0, 1);
	FVector3 res = sideVec * offset->X + upVec * offset->Y;
	mat->Translate(res.X, res.Z, res.Y);
}

bool HWSprite::CalculateVertices(HWDrawInfo* di, FVector3* v, DVector3* vp)
{
	float pixelstretch = 1.2;
	if (actor && actor->Level)
		pixelstretch = actor->Level->pixelstretch;
	else if (particle && particle->subsector && particle->subsector->sector && particle->subsector->sector->Level)
		pixelstretch = particle->subsector->sector->Level->pixelstretch;
	// float pixelstretch = di->Level->pixelstretch;

	FVector3 center = FVector3((x1 + x2) * 0.5, (y1 + y2) * 0.5, (z1 + z2) * 0.5);
	const auto& HWAngles = di->Viewpoint.HWAngles;
	Matrix3x4 mat;
	if (actor != nullptr && (actor->renderflags & RF_SPRITETYPEMASK) == RF_FLATSPRITE)
	{
		// [MC] Rotate around the center or offsets given to the sprites.
		// Counteract any existing rotations, then rotate the angle.
		// Tilt the actor up or down based on pitch (increase 'somersaults' forward).
		// Then counteract the roll and DO A BARREL ROLL.

		mat.MakeIdentity();
		FAngle pitch = FAngle::fromDeg(-Angles.Pitch.Degrees());
		pitch.Normalized180();

		mat.Translate(x, z, y);
		mat.Rotate(0, 1, 0, 270. - Angles.Yaw.Degrees());
		mat.Rotate(1, 0, 0, pitch.Degrees());

		if (actor->renderflags & RF_ROLLCENTER)
		{
			mat.Translate(center.X - x, 0, center.Y - y);
			mat.Rotate(0, 1, 0, - Angles.Roll.Degrees());
			mat.Translate(-center.X, -z, -center.Y);
		}
		else
		{
			mat.Rotate(0, 1, 0, - Angles.Roll.Degrees());
			mat.Translate(-x, -z, -y);
		}
		v[0] = mat * FVector3(x2, z, y2);
		v[1] = mat * FVector3(x1, z, y2);
		v[2] = mat * FVector3(x2, z, y1);
		v[3] = mat * FVector3(x1, z, y1);

		return true;
	}
	
	// [BB] Billboard stuff
	const bool drawWithXYBillboard = ((particle && gl_billboard_particles && !(particle->flags & SPF_NO_XY_BILLBOARD)) || (!(actor && actor->renderflags & RF_FORCEYBILLBOARD)
		//&& di->mViewActor != nullptr
		&& (gl_billboard_mode == 1 || (actor && actor->renderflags & RF_FORCEXYBILLBOARD))));

	const bool drawBillboardFacingCamera = hw_force_cambbpref ? gl_billboard_faces_camera :
		gl_billboard_faces_camera
		|| ((actor && (!(actor->renderflags2 & RF2_BILLBOARDNOFACECAMERA) && (actor->renderflags2 & RF2_BILLBOARDFACECAMERA)))
		|| (particle && particle->texture.isValid() && (!(particle->flags & SPF_NOFACECAMERA) && (particle->flags & SPF_FACECAMERA))));

	// [Nash] has +ROLLSPRITE
	const bool drawRollSpriteActor = (actor != nullptr && actor->renderflags & RF_ROLLSPRITE);
	const bool drawRollParticle = (particle != nullptr && particle->flags & SPF_ROLL);
	const bool doRoll = (drawRollSpriteActor || drawRollParticle);

	// [fgsfds] check sprite type mask
	uint32_t spritetype = (uint32_t)-1;
	if (actor != nullptr) spritetype = actor->renderflags & RF_SPRITETYPEMASK;

	// [Nash] is a flat sprite
	const bool isWallSprite = (actor != nullptr) && (spritetype == RF_WALLSPRITE);
	const bool useOffsets = ((actor != nullptr) && !(actor->renderflags & RF_ROLLCENTER)) || (particle && !(particle->flags & SPF_ROLLCENTER));

	FVector2 offset = FVector2( offx, offy );
	float xx = -center.X + x;
	float yy = -center.Y + y;
	float zz = -center.Z + z;
	// [Nash] check for special sprite drawing modes
	if (drawWithXYBillboard || drawBillboardFacingCamera || isWallSprite)
	{
		mat.MakeIdentity();
		mat.Translate(center.X, center.Z, center.Y); // move to sprite center
		mat.Scale(1.0, 1.0/pixelstretch, 1.0);	// unstretch sprite by level aspect ratio

		// [MC] Sprite offsets.
		if (!offset.isZero())
			HandleSpriteOffsets(&mat, &HWAngles, &offset, true);

		// Order of rotations matters. Perform yaw rotation (Y, face camera) before pitch (X, tilt up/down).
		if (drawBillboardFacingCamera && !isWallSprite)
		{
			// [CMB] Rotate relative to camera XY position, not just camera direction,
			// which is nicer in VR
			float xrel = center.X - vp->X;
			float yrel = center.Y - vp->Y;
			float absAngleDeg = atan2(-yrel, xrel) * (180 / M_PI);
			float counterRotationDeg = 270. - HWAngles.Yaw.Degrees(); // counteracts existing sprite rotation
			float relAngleDeg = counterRotationDeg + absAngleDeg;

			mat.Rotate(0, 1, 0, relAngleDeg);
		}

		// [fgsfds] calculate yaw vectors
		float rollDegrees = doRoll ? Angles.Roll.Degrees() : 0;
		float angleRad = (FAngle::fromDeg(270.) - HWAngles.Yaw).Radians();

		// [fgsfds] Rotate the sprite about the sight vector (roll) 
		if (isWallSprite)
		{
			float yawvecX = Angles.Yaw.Cos();
			float yawvecY = Angles.Yaw.Sin();
			mat.Rotate(0, 1, 0, 0);
			if (drawRollSpriteActor)
			{

				if (useOffsets) mat.Translate(xx, zz, yy);
				mat.Rotate(yawvecX, 0, yawvecY, rollDegrees);
				if (useOffsets) mat.Translate(-xx, -zz, -yy);
			}
		}
		else if (doRoll)
		{
			if (useOffsets) mat.Translate(xx, zz, yy);
			if (drawWithXYBillboard)
			{
				mat.Rotate(-sin(angleRad), 0, cos(angleRad), -HWAngles.Pitch.Degrees());
			}
			mat.Rotate(cos(angleRad), 0, sin(angleRad), rollDegrees);
			if (useOffsets) mat.Translate(-xx, -zz, -yy);
		}
		else if (drawWithXYBillboard)
		{
			// Rotate the sprite about the vector starting at the center of the sprite
			// triangle strip and with direction orthogonal to where the player is looking
			// in the x/y plane.
			mat.Rotate(-sin(angleRad), 0, cos(angleRad), -HWAngles.Pitch.Degrees());
		}

		mat.Scale(1.0, pixelstretch, 1.0);	// stretch sprite by level aspect ratio
		mat.Translate(-center.X, -center.Z, -center.Y); // retreat from sprite center

		v[0] = mat * FVector3(x1, z1, y1);
		v[1] = mat * FVector3(x2, z1, y2);
		v[2] = mat * FVector3(x1, z2, y1);
		v[3] = mat * FVector3(x2, z2, y2);
	}
	else // traditional "Y" billboard mode
	{
		if (doRoll || !offset.isZero() || (actor && (actor->renderflags2 & RF2_ISOMETRICSPRITES)))
		{
			mat.MakeIdentity();

			if (!offset.isZero())
				HandleSpriteOffsets(&mat, &HWAngles, &offset, false);
			
			if (doRoll)
			{
				// Compute center of sprite
				float angleRad = (FAngle::fromDeg(270.) - HWAngles.Yaw).Radians();
				float rollDegrees = Angles.Roll.Degrees();

				mat.Translate(center.X, center.Z, center.Y);
				mat.Scale(1.0, 1.0/pixelstretch, 1.0);	// unstretch sprite by level aspect ratio
				if (useOffsets) mat.Translate(xx, zz, yy);
				mat.Rotate(cos(angleRad), 0, sin(angleRad), rollDegrees);
				if (useOffsets) mat.Translate(-xx, -zz, -yy);
				mat.Scale(1.0, pixelstretch, 1.0);	// stretch sprite by level aspect ratio
				mat.Translate(-center.X, -center.Z, -center.Y);
			}

			if (actor && (actor->renderflags2 & RF2_ISOMETRICSPRITES) && di->Viewpoint.IsOrtho())
			{
				float angleRad = (FAngle::fromDeg(270.) - HWAngles.Yaw).Radians();
				mat.Translate(center.X, center.Z, center.Y);
				mat.Translate(0.0, z2 - center.Z, 0.0);
				mat.Rotate(-sin(angleRad), 0, cos(angleRad), -actor->isotheta);
				mat.Translate(0.0, center.Z - z2, 0.0);
				mat.Translate(-center.X, -center.Z, -center.Y);
			}

			v[0] = mat * FVector3(x1, z1, y1);
			v[1] = mat * FVector3(x2, z1, y2);
			v[2] = mat * FVector3(x1, z2, y1);
			v[3] = mat * FVector3(x2, z2, y2);
			
		}
		else
		{
			v[0] = FVector3(x1, z1, y1);
			v[1] = FVector3(x2, z1, y2);
			v[2] = FVector3(x1, z2, y1);
			v[3] = FVector3(x2, z2, y2);
		}
		
	}
	return false;
}

//==========================================================================
//
// 
//
//==========================================================================

inline void HWSprite::PutSprite(HWDrawInfo *di, FRenderState& state, bool translucent)
{
	// That's a lot of checks...
	if ((get_gl_spritelight() > 0 || (modelframe && !modelframe->isVoxel && !(modelframeflags & MDL_NOPERPIXELLIGHTING))) && RenderStyle.BlendOp != STYLEOP_Shadow && gl_light_sprites && di->Level->HasDynamicLights && !di->isFullbrightScene() && !fullbright)
	{
		di->GetDynSpriteLightList(actor, gl_light_particles ? particle : nullptr, (gl_light_particles && spr != nullptr) ? &spr->StaticLightsTraceCache : nullptr, lightdata, modelframe && !modelframe->isVoxel);
		dynlightindex = state.UploadLights(lightdata);
	}
	else
		dynlightindex = -1;

	vertexindex = -1;
	di->AddSprite(this, translucent);
}

//==========================================================================
//
// 
//
//==========================================================================

void HWSprite::CreateVertices(HWDrawInfo *di, FRenderState& state)
{
	if (modelframe == nullptr)
	{
		FVector3 v[4];
		polyoffset = CalculateVertices(di, v, &di->Viewpoint.Pos);
		auto vert = state.AllocVertices(4);
		auto vp = vert.first;
		vertexindex = vert.second;

		vp[0].Set(v[0][0], v[0][1], v[0][2], ul, vt);
		vp[1].Set(v[1][0], v[1][1], v[1][2], ur, vt);
		vp[2].Set(v[2][0], v[2][1], v[2][2], ul, vb);
		vp[3].Set(v[3][0], v[3][1], v[3][2], ur, vb);
	}

}


//==========================================================================
//
// 
//
//==========================================================================

void HWSprite::SplitSprite(HWDrawInfo *di, FRenderState& state, sector_t * frontsector, bool translucent)
{
	HWSprite copySprite;
	double lightbottom;
	unsigned int i;
	bool put=false;
	TArray<lightlist_t> & lightlist=frontsector->e->XFloor.lightlist;

	for(i=0;i<lightlist.Size();i++)
	{
		// Particles don't go through here so we can safely assume that actor is not nullptr
		if (i<lightlist.Size()-1) lightbottom=lightlist[i+1].plane.ZatPoint(actor);
		else lightbottom=frontsector->floorplane.ZatPoint(actor);

		if (lightbottom<z2) lightbottom=z2;

		if (lightbottom<z1)
		{
			copySprite=*this;
			copySprite.lightlevel = hw_ClampLight(*lightlist[i].p_lightlevel);
			copySprite.Colormap.CopyLight(lightlist[i].extra_colormap);

			if (di->Level->flags3 & LEVEL3_NOCOLOREDSPRITELIGHTING)
			{
				copySprite.Colormap.Decolorize();
			}

			if (!ThingColor.isWhite())
			{
				copySprite.Colormap.LightColor.r = (copySprite.Colormap.LightColor.r*ThingColor.r) >> 8;
				copySprite.Colormap.LightColor.g = (copySprite.Colormap.LightColor.g*ThingColor.g) >> 8;
				copySprite.Colormap.LightColor.b = (copySprite.Colormap.LightColor.b*ThingColor.b) >> 8;
			}

			z1=copySprite.z2=lightbottom;
			vt=copySprite.vb=copySprite.vt+ 
				(lightbottom-copySprite.z1)*(copySprite.vb-copySprite.vt)/(z2-copySprite.z1);
			copySprite.PutSprite(di, state, translucent);
			put=true;
		}
	}
}

//==========================================================================
//
// 
//
//==========================================================================

void HWSprite::PerformSpriteClipAdjustment(AActor *thing, const DVector2 &thingpos, float spriteheight)
{
	const float NO_VAL = 100000000.0f;
	bool clipthing = (thing->player || thing->flags3&MF3_ISMONSTER || thing->IsKindOf(NAME_Inventory)) && (thing->flags&MF_ICECORPSE || !(thing->flags&MF_CORPSE));
	bool smarterclip = !clipthing && gl_spriteclip == 3;
	if ((clipthing || gl_spriteclip > 1) && !(thing->flags2 & MF2_FLOATBOB))
	{

		float btm = NO_VAL;
		float top = -NO_VAL;
		extsector_t::xfloor &x = thing->Sector->e->XFloor;

		if (x.ffloors.Size())
		{
			for (unsigned int i = 0; i < x.ffloors.Size(); i++)
			{
				F3DFloor * ff = x.ffloors[i];
				if (ff->flags & FF_THISINSIDE) continue;	// only relevant for software rendering.
				float floorh = ff->top.plane->ZatPoint(thingpos);
				float ceilingh = ff->bottom.plane->ZatPoint(thingpos);
				if (floorh == thing->floorz)
				{
					btm = floorh;
				}
				if (ceilingh == thing->ceilingz)
				{
					top = ceilingh;
				}
				if (btm != NO_VAL && top != -NO_VAL)
				{
					break;
				}
			}
		}
		else if (thing->Sector->GetHeightSec())
		{
			if (thing->flags2&MF2_ONMOBJ && thing->floorz ==
				thing->Sector->heightsec->floorplane.ZatPoint(thingpos))
			{
				btm = thing->floorz;
				top = thing->ceilingz;
			}
		}
		if (btm == NO_VAL)
			btm = thing->Sector->floorplane.ZatPoint(thing) - thing->Floorclip;
		if (top == NO_VAL)
			top = thing->Sector->ceilingplane.ZatPoint(thingpos);

		// +/-1 to account for the one pixel empty frame around the sprite.
		float diffb = (z2+1) - btm;
		float difft = (z1-1) - top;
		if (diffb >= 0 /*|| !gl_sprite_clip_to_floor*/) diffb = 0;
		// Adjust sprites clipping into ceiling and adjust clipping adjustment for tall graphics
		if (smarterclip)
		{
			// Reduce slightly clipping adjustment of corpses
			if (thing->flags & MF_CORPSE || spriteheight > fabs(diffb))
			{
				float ratio = clamp<float>((fabs(diffb) * (float)gl_sclipfactor / (spriteheight + 1)), 0.5, 1.0);
				diffb *= ratio;
			}
			if (!diffb)
			{
				if (difft <= 0) difft = 0;
				if (difft >= (float)gl_sclipthreshold)
				{
					// dumb copy of the above.
					if (!(thing->flags3&MF3_ISMONSTER) || (thing->flags&MF_NOGRAVITY) || (thing->flags&MF_CORPSE) || difft > (float)gl_sclipthreshold)
					{
						difft = 0;
					}
				}
				if (spriteheight > fabs(difft))
				{
					float ratio = clamp<float>((fabs(difft) * (float)gl_sclipfactor / (spriteheight + 1)), 0.5, 1.0);
					difft *= ratio;
				}
				z2 -= difft;
				z1 -= difft;
			}
		}
		if (diffb <= (0 - (float)gl_sclipthreshold))	// such a large displacement can't be correct! 
		{
			// for living monsters standing on the floor allow a little more.
			if (!(thing->flags3&MF3_ISMONSTER) || (thing->flags&MF_NOGRAVITY) || (thing->flags&MF_CORPSE) || diffb < (-1.8*(float)gl_sclipthreshold))
			{
				diffb = 0;
			}
		}
		z2 -= diffb;
		z1 -= diffb;
	}
}

//==========================================================================
//
// 
//
//==========================================================================

void HWSprite::Process(HWDrawInfo *di, FRenderState& state, AActor* thing, sector_t * sector, area_t in_area, int thruportal, bool isSpriteShadow)
{
	sector_t rs;
	sector_t * rendersector;

	if (thing == nullptr)
		return;

	// [ZZ] allow CustomSprite-style direct picnum specification
	bool isPicnumOverride = thing->picnum.isValid();

	bool isFogball = thing->IsKindOf(NAME_Fogball);

	// Don't waste time projecting sprites that are definitely not visible.
	if ((thing->sprite == 0 && !isPicnumOverride && !isFogball) || !thing->IsVisibleToPlayer() || ((thing->renderflags & RF_MASKROTATION) && !thing->IsInsideVisibleAngles()))
	{
		return;
	}

	if (isFogball)
	{
		Fogball fogball;
		fogball.Position = FVector3(thing->PosRelative(r_viewpoint.sector));
		fogball.Radius = (float)thing->args[3];
		fogball.Color = FVector3(powf(thing->args[0] * (1.0f / 255.0f), 2.2), powf(thing->args[1] * (1.0f / 255.0f), 2.2), powf(thing->args[2] * (1.0f / 255.0f), 2.2));
		fogball.Fog = (float)thing->Alpha;
		di->Fogballs.Push(fogball);
		return;
	}

	if (thing->IsKindOf(NAME_Corona))
	{
		Coronas.SortedInsertUnique(thing);
		return;
	}

	const auto &vp = di->Viewpoint;
	AActor *camera = vp.camera;

	if (thing->renderflags & RF_INVISIBLE || !thing->RenderStyle.IsVisible(thing->Alpha))
	{
		if (!(thing->flags & MF_STEALTH) || !di->isStealthVision() || thing == camera)
			return;
	}

	// check renderrequired vs ~r_rendercaps, if anything matches we don't support that feature,
	// check renderhidden vs r_rendercaps, if anything matches we do support that feature and should hide it.
	if ((!r_debug_disable_vis_filter && !!(thing->RenderRequired & ~r_renderercaps)) ||
		(!!(thing->RenderHidden & r_renderercaps)))
		return;

	int spritenum = thing->sprite;
	DVector2 sprscale(thing->Scale.X, thing->Scale.Y);
	if (thing->player != nullptr)
	{
		P_CheckPlayerSprite(thing, spritenum, sprscale);
	}

	// [RH] Interpolate the sprite's position to make it look smooth
	DVector3 thingpos = thing->InterpolatedPosition(vp.TicFrac);
	if (thruportal == 1) thingpos += di->Level->Displacements.getOffset(thing->Sector->PortalGroup, sector->PortalGroup);

	AActor *viewmaster = thing;
	if ((thing->flags8 & MF8_MASTERNOSEE) && thing->master != nullptr)
	{
		viewmaster = thing->master;
	}

	// [Nash] filter visibility in mirrors
	bool isInMirror = di->mCurrentPortal && (di->mCurrentPortal->mState->MirrorFlag > 0 || di->mCurrentPortal->mState->PlaneMirrorFlag > 0);
	if (thing->renderflags2 & RF2_INVISIBLEINMIRRORS && isInMirror)
	{
		return;
	}
	else if (thing->renderflags2 & RF2_ONLYVISIBLEINMIRRORS && !isInMirror)
	{
		return;
	}
	// Some added checks if the camera actor is not supposed to be seen. It can happen that some portal setup has this actor in view in which case it may not be skipped here
	if (viewmaster == camera && !vp.showviewer)
	{
		if (vp.bForceNoViewer || (viewmaster->player && viewmaster->player->crossingPortal)) return;
		DVector3 vieworigin = viewmaster->Pos();
		if (thruportal == 1) vieworigin += di->Level->Displacements.getOffset(viewmaster->Sector->PortalGroup, sector->PortalGroup);
		if (fabs(vieworigin.X - vp.ActorPos.X) < 2 && fabs(vieworigin.Y - vp.ActorPos.Y) < 2) return;

		// Necessary in order to prevent sprite pop-ins with viewpos and models. 
		auto* sec = viewmaster->Sector;
		if (sec && !sec->PortalBlocksMovement(sector_t::ceiling))
		{
			double zh = sec->GetPortalPlaneZ(sector_t::ceiling);
			double top = (viewmaster->player ? max<double>(viewmaster->player->viewz, viewmaster->Top()) + 1 : viewmaster->Top());
			if (viewmaster->Z() < zh && top >= zh)
				return;
		}
	}
	// Thing is invisible if close to the camera.
	if (viewmaster->renderflags & RF_MAYBEINVISIBLE)
	{
		DVector3 viewpos = viewmaster->InterpolatedPosition(vp.TicFrac);
		if (thruportal == 1) viewpos += di->Level->Displacements.getOffset(viewmaster->Sector->PortalGroup, sector->PortalGroup);
		if (fabs(viewpos.X - vp.Pos.X) < 32 && fabs(viewpos.Y - vp.Pos.Y) < 32) return;
	}

	modelframe = isPicnumOverride ? nullptr : FindModelFrame(thing, spritenum, thing->frame, !!(thing->flags & MF_DROPPED));
	modelframeflags = modelframe ? modelframe->getFlags(thing->modelData) : 0;

	// Too close to the camera. This doesn't look good if it is a sprite.
	if (fabs(thingpos.X - vp.Pos.X) < 2 && fabs(thingpos.Y - vp.Pos.Y) < 2
		&& vp.Pos.Z >= thingpos.Z - 2 && vp.Pos.Z <= thingpos.Z + thing->Height + 2
		&& !thing->Vel.isZero() && !modelframe) // exclude vertically moving objects from this check.
	{
		return;
	}

	// don't draw first frame of a player missile
	if (thing->flags&MF_MISSILE)
	{
		if (!(thing->flags7 & MF7_FLYCHEAT) && thing->target == vp.ViewActor && vp.ViewActor != nullptr)
		{
			double speed = thing->Vel.Length();
			if (speed >= thing->target->radius / 2)
			{
				double clipdist = clamp(thing->Speed, thing->target->radius, thing->target->radius * 2);
				if ((thingpos - vp.Pos).LengthSquared() < clipdist * clipdist) return;
			}
		}
		thing->flags7 |= MF7_FLYCHEAT;	// do this only once for the very first frame, but not if it gets into range again.
	}

	if (thruportal != 2 && di->mClipPortal != nullptr)
	{
		int clipres = di->mClipPortal->ClipPoint(thingpos.XY());
		if (clipres == PClip_InFront) return;
	}
	// disabled because almost none of the actual game code is even remotely prepared for this. If desired, use the INTERPOLATE flag.
	if (thing->renderflags & RF_INTERPOLATEANGLES)
		Angles = thing->InterpolatedAngles(vp.TicFrac);
	else
		Angles = thing->Angles;

	if (sector->sectornum != thing->Sector->sectornum && !thruportal)
	{
		// This cannot create a copy in the fake sector cache because it'd interfere with the main thread, so provide a local buffer for the copy.
		// Adding synchronization for this one case would cost more than it might save if the result here could be cached.
		rendersector = hw_FakeFlat(di->drawctx, thing->Sector, in_area, false, &rs);
	}
	else
	{
		rendersector = sector;
	}
	topclip = rendersector->PortalBlocksMovement(sector_t::ceiling) ? LARGE_VALUE : rendersector->GetPortalPlaneZ(sector_t::ceiling);
	bottomclip = rendersector->PortalBlocksMovement(sector_t::floor) ? -LARGE_VALUE : rendersector->GetPortalPlaneZ(sector_t::floor);

	uint32_t spritetype = (thing->renderflags & RF_SPRITETYPEMASK);
	x = thingpos.X + thing->WorldOffset.X;
	z = thingpos.Z + thing->WorldOffset.Z;
	y = thingpos.Y + thing->WorldOffset.Y;
	if (spritetype == RF_FACESPRITE) z -= thing->Floorclip; // wall and flat sprites are to be considered di->Level-> geometry so this may not apply.

	// snap shadow Z to the floor
	if (isSpriteShadow)
	{
		z = thing->floorz;
	}
	// [RH] Make floatbobbing a renderer-only effect.
	else
	{
		float fz = thing->GetBobOffset(vp.TicFrac);
		z += fz;
	}

	// don't bother drawing sprite shadows if this is a model (it will never look right)
	if (modelframe && isSpriteShadow)
	{
		return;
	}
	if (!modelframe)
	{
		bool mirror = false;
		DAngle ang = (thingpos - vp.Pos).Angle();
		if (di->Viewpoint.IsOrtho()) ang = vp.Angles.Yaw;
		FTextureID patch;
		// [ZZ] add direct picnum override
		if (isPicnumOverride)
		{
			// Animate picnum overrides.
			auto tex = TexMan.GetGameTexture(thing->picnum, true);
			if (tex == nullptr) return;

			if (tex->GetRotations() != 0xFFFF)
			{
				// choose a different rotation based on player view
				spriteframe_t* sprframe = &SpriteFrames[tex->GetRotations()];
				DAngle sprang = thing->GetSpriteAngle(ang, vp.TicFrac);
				angle_t rot;
				if (sprframe->Texture[0] == sprframe->Texture[1])
				{
					if (thing->flags7 & MF7_SPRITEANGLE)
						rot = (thing->SpriteAngle + DAngle::fromDeg(45.0 / 2 * 9)).BAMs() >> 28;
					else
						rot = (sprang - (thing->Angles.Yaw + thing->SpriteRotation) + DAngle::fromDeg(45.0 / 2 * 9)).BAMs() >> 28;
				}
				else
				{
					if (thing->flags7 & MF7_SPRITEANGLE)
						rot = (thing->SpriteAngle + DAngle::fromDeg(45.0 / 2 * 9 - 180.0 / 16)).BAMs() >> 28;
					else
						rot = (sprang - (thing->Angles.Yaw + thing->SpriteRotation) + DAngle::fromDeg(45.0 / 2 * 9 - 180.0 / 16)).BAMs() >> 28;
				}
				auto picnum = sprframe->Texture[rot];
				if (sprframe->Flip & (1 << rot))
				{
					mirror = true;
				}
			}

			patch =  tex->GetID();
		}
		else
		{
			DAngle sprangle;
			int rot;
			if (!(thing->renderflags & RF_FLATSPRITE) || thing->flags7 & MF7_SPRITEANGLE)
			{
				sprangle = thing->GetSpriteAngle(ang, vp.TicFrac);
				rot = -1;
			}
			else
			{
				// Flat sprites cannot rotate in a predictable manner.
				sprangle = nullAngle;
				rot = 0;
			}
			patch = sprites[spritenum].GetSpriteFrame(thing->frame, rot, sprangle, &mirror, !!(thing->renderflags & RF_SPRITEFLIP));
		}

		if (!patch.isValid()) return;
		int type = thing->renderflags & RF_SPRITETYPEMASK;
		auto tex = TexMan.GetGameTexture(patch, false);
		if (!tex || !tex->isValid()) return;
		auto& spi = tex->GetSpritePositioning(type == RF_FACESPRITE);

		offx = (float)thing->GetSpriteOffset(false);
		offy = (float)thing->GetSpriteOffset(true);

		vt = spi.GetSpriteVT();
		vb = spi.GetSpriteVB();
		if (thing->renderflags & RF_YFLIP) std::swap(vt, vb);

		auto r = spi.GetSpriteRect();

		// [SP] SpriteFlip
		if (thing->renderflags & RF_SPRITEFLIP)
			thing->renderflags ^= RF_XFLIP;

		if (mirror ^ !!(thing->renderflags & RF_XFLIP))
		{
			r.left = -r.width - r.left;	// mirror the sprite's x-offset
			ul = spi.GetSpriteUL();
			ur = spi.GetSpriteUR();
		}
		else
		{
			ul = spi.GetSpriteUR();
			ur = spi.GetSpriteUL();
		}

		texture = tex;
		if (!texture || !texture->isValid())
			return;

		if (thing->renderflags & RF_SPRITEFLIP) // [SP] Flip back
			thing->renderflags ^= RF_XFLIP;

		// If sprite is isometric, do both vertical scaling and partial rotation to face the camera to compensate for Y-billboarding.
		// Using just rotation (about z=0) might cause tall+slender (high aspect ratio) sprites to clip out of collision box
		// at the top and clip into whatever is behind them from the viewpoint's perspective. - [DVR]
		thing->isoscaleY = 1.0;
		thing->isotheta = vp.HWAngles.Pitch.Degrees();
		if (thing->renderflags2 & RF2_ISOMETRICSPRITES)
		{
			float floordist = thing->radius * vp.floordistfact;
			floordist -= 0.5 * r.width * vp.cotfloor;
			float sineisotheta = floordist / r.height;
			double scl = g_sqrt( 1.0 + sineisotheta * sineisotheta - 2.0 * vp.PitchSin * sineisotheta );
			if ((thing->radius > 0.0) && (scl > fabs(vp.PitchCos)))
			{
				thing->isoscaleY = scl / ( fabs(vp.PitchCos) > 0.01 ? fabs(vp.PitchCos) : 0.01 );
				thing->isotheta = 180.0 * asin( sineisotheta / thing->isoscaleY ) / M_PI;
			}
		}

		r.Scale(sprscale.X, isSpriteShadow ? sprscale.Y * 0.15 * thing->isoscaleY : sprscale.Y * thing->isoscaleY);

		if (((thing->renderflags & RF_ROLLSPRITE) || (thing->renderflags2 & RF2_SQUAREPIXELS)) && !(thing->renderflags2 & RF2_STRETCHPIXELS))
		{
			double ps = di->Level->pixelstretch;
			double mult = 1.0 / sqrt(ps); // shrink slightly
			r.Scale(mult * ps, mult);
		}

		float rightfac = -r.left;
		float leftfac = rightfac - r.width;
		z1 = z - r.top;
		z2 = z1 - r.height;

		float spriteheight = sprscale.Y * r.height * thing->isoscaleY;

		// Tests show that this doesn't look good for many decorations and corpses
		if (spriteheight > 0 && gl_spriteclip > 0 && (thing->renderflags & RF_SPRITETYPEMASK) == RF_FACESPRITE)
		{
			PerformSpriteClipAdjustment(thing, thingpos.XY(), spriteheight);
		}

		switch (spritetype)
		{
		case RF_FACESPRITE:
		{
			float viewvecX = vp.ViewVector.X;
			float viewvecY = vp.ViewVector.Y;

			x1 = x - viewvecY*leftfac;
			x2 = x - viewvecY*rightfac;
			y1 = y + viewvecX*leftfac;
			y2 = y + viewvecX*rightfac;
			if (thing->renderflags2 & RF2_ISOMETRICSPRITES) // If sprites are drawn from an isometric perspective
			{
				x1 -= viewvecX * thing->radius * MY_SQRT2;
				x2 -= viewvecX * thing->radius * MY_SQRT2;
				y1 -= viewvecY * thing->radius * MY_SQRT2;
				y2 -= viewvecY * thing->radius * MY_SQRT2;
			}
			break;
		}
		case RF_FLATSPRITE:
		{
			float bottomfac = -r.top;
			float topfac = bottomfac - r.height;

			x1 = x + leftfac;
			x2 = x + rightfac;
			y1 = y - topfac;
			y2 = y - bottomfac;
			// [MC] Counteract in case of any potential problems. Tests so far haven't
			// shown any outstanding issues but that doesn't mean they won't appear later
			// when more features are added.
			z1 += offy;
			z2 += offy;
			break;
		}
		case RF_WALLSPRITE:
		{
			float viewvecX = Angles.Yaw.Cos();
			float viewvecY = Angles.Yaw.Sin();

			x1 = x + viewvecY*leftfac;
			x2 = x + viewvecY*rightfac;
			y1 = y - viewvecX*leftfac;
			y2 = y - viewvecX*rightfac;
			break;
		}
		}
	}
	else
	{
		x1 = x2 = x;
		y1 = y2 = y;
		z1 = z2 = z;
		texture = nullptr;
	}

	depth = (float)((x - vp.Pos.X) * vp.TanCos + (y - vp.Pos.Y) * vp.TanSin);
	if(thing->renderflags2 & RF2_ISOMETRICSPRITES) depth = depth * vp.PitchCos - vp.PitchSin * z2; // Helps with stacking actors with small xy offsets
	if (isSpriteShadow) depth += 1.f/65536.f; // always sort shadows behind the sprite.

	if (gl_spriteclip == -1 && (thing->renderflags & RF_SPRITETYPEMASK) == RF_FACESPRITE) // perform anamorphosis
	{
		float minbias = r_spriteclipanamorphicminbias;
		minbias = clamp(minbias, 0.3f, 1.0f);

		float btm = thing->Sector->floorplane.ZatPoint(thing) - thing->Floorclip;
		float top = thing->Sector->ceilingplane.ZatPoint(thingpos);

		float vbtm = thing->Sector->floorplane.ZatPoint(vp.Pos);
		float vtop = thing->Sector->ceilingplane.ZatPoint(vp.Pos);

		float vpx = vp.Pos.X;
		float vpy = vp.Pos.Y;
		float vpz = vp.Pos.Z;

		float tpx = thingpos.X;
		float tpy = thingpos.Y;
		float tpz = thingpos.Z;

		if (!(r_debug_nolimitanamorphoses))
		{
			// this should help prevent clipping through walls ...
			float objradiusbias = 1.f - thing->radius / sqrt((vpx - tpx) * (vpx - tpx) + (vpy - tpy) * (vpy - tpy));
			minbias = max(minbias, objradiusbias);
		}

		float bintersect, tintersect;
		if (z2 < vpz && vbtm < vpz)
			bintersect = min((btm - vpz) / (z2 - vpz), (vbtm - vpz) / (z2 - vpz));
		else
			bintersect = 1.0;

		if (z1 > vpz && vtop > vpz)
			tintersect = min((top - vpz) / (z1 - vpz), (vtop - vpz) / (z1 - vpz));
		else
			tintersect = 1.0;

		if (thing->waterlevel >= 1 && thing->waterlevel <= 2)
			bintersect = tintersect = 1.0f;

		float spbias = clamp(min(bintersect, tintersect), minbias, 1.0f);
		float vpbias = 1.0 - spbias;
		x1 = x1 * spbias + vpx * vpbias;
		y1 = y1 * spbias + vpy * vpbias;
		z1 = z1 * spbias + vpz * vpbias;
		x2 = x2 * spbias + vpx * vpbias;
		y2 = y2 * spbias + vpy * vpbias;
		z2 = z2 * spbias + vpz * vpbias;		
	}

	// light calculation

	bool enhancedvision = false;

	// allow disabling of the fullbright flag by a brightmap definition
	// (e.g. to do the gun flashes of Doom's zombies correctly.
	fullbright = (thing->flags5 & MF5_BRIGHT) ||
		((thing->renderflags & RF_FULLBRIGHT) && (!texture || !texture->isFullbrightDisabled()));

	if (fullbright)	lightlevel = 255;
	else lightlevel = hw_ClampLight(thing->GetLightLevel(rendersector));

	foglevel = (uint8_t)clamp<short>(rendersector->lightlevel, 0, 255); // this *must* use the sector's light level or the fog will just look bad.

	lightlevel = rendersector->CheckSpriteGlow(lightlevel, thingpos);

	ThingColor = (thing->RenderStyle.Flags & STYLEF_ColorIsFixed) ? thing->fillcolor : 0xffffff;
	ThingColor.a = 255;
	RenderStyle = thing->RenderStyle;

	// colormap stuff is a little more complicated here...
	if (di->isFullbrightScene())
	{
		enhancedvision = di->isStealthVision();

		Colormap.Clear();

		if (di->isNightvision())
		{
			if ((thing->IsKindOf(NAME_Inventory) || thing->flags3&MF3_ISMONSTER || thing->flags&MF_MISSILE || thing->flags&MF_CORPSE))
			{
				RenderStyle.Flags |= STYLEF_InvertSource;
			}
		}
	}
	else
	{
		Colormap = rendersector->Colormap;
		if (fullbright)
		{
			if (rendersector == &di->Level->sectors[rendersector->sectornum] || in_area != area_below)
				// under water areas keep their color for fullbright objects
			{
				// Only make the light white but keep everything else (fog, desaturation and Boom colormap.)
				Colormap.MakeWhite();
			}
			else
			{
				// Keep the color, but brighten things a bit so that a difference can be seen.
				Colormap.LightColor.r = (3 * Colormap.LightColor.r + 0xff) / 4;
				Colormap.LightColor.g = (3 * Colormap.LightColor.g + 0xff) / 4;
				Colormap.LightColor.b = (3 * Colormap.LightColor.b + 0xff) / 4;
			}
		}
		else if (di->Level->flags3 & LEVEL3_NOCOLOREDSPRITELIGHTING)
		{
			Colormap.Decolorize();
		}
	}

	translation = thing->Translation;

	OverrideShader = -1;
	trans = thing->Alpha;
	hw_styleflags = STYLEHW_Normal;

	if (RenderStyle.BlendOp >= STYLEOP_Fuzz && RenderStyle.BlendOp <= STYLEOP_FuzzOrRevSub)
	{
		RenderStyle.CheckFuzz();
		if (RenderStyle.BlendOp == STYLEOP_Fuzz)
		{
			if (gl_fuzztype != 0 && !(RenderStyle.Flags & STYLEF_InvertSource))
			{
				RenderStyle = LegacyRenderStyles[STYLE_Translucent];
				OverrideShader = SHADER_NoTexture + gl_fuzztype;
				trans = 0.99f;	// trans may not be 1 here
				hw_styleflags = STYLEHW_NoAlphaTest;
			}
			else
			{
				// Without shaders only the standard effect is available.
				RenderStyle.BlendOp = STYLEOP_Shadow;
			}
		}
	}

	if (RenderStyle.Flags & STYLEF_TransSoulsAlpha)
	{
		trans = transsouls;
	}
	else if (RenderStyle.Flags & STYLEF_Alpha1)
	{
		trans = 1.f;
	}
	if (r_UseVanillaTransparency)
	{
		// [SP] "canonical transparency" - with the flip of a CVar, disable transparency for Doom objects,
		//   and disable 'additive' translucency for certain objects from other games.
		if (thing->renderflags & RF_ZDOOMTRANS)
		{
			trans = 1.f;
			RenderStyle.BlendOp = STYLEOP_Add;
			RenderStyle.SrcAlpha = STYLEALPHA_One;
			RenderStyle.DestAlpha = STYLEALPHA_Zero;
		}
	}
	if (trans >= 1.f - FLT_EPSILON && RenderStyle.BlendOp != STYLEOP_Shadow && (
		(RenderStyle.SrcAlpha == STYLEALPHA_One && RenderStyle.DestAlpha == STYLEALPHA_Zero) ||
		(RenderStyle.SrcAlpha == STYLEALPHA_Src && RenderStyle.DestAlpha == STYLEALPHA_InvSrc)
		))
	{
		// This is a non-translucent sprite (i.e. STYLE_Normal or equivalent)
		trans = 1.f;

		if (!gl_sprite_blend || modelframe ||
			(thing->renderflags & (RF_FLATSPRITE | RF_WALLSPRITE)) ||
			(hw_force_cambbpref ? gl_billboard_faces_camera :
			(gl_billboard_faces_camera && !(thing->renderflags2 & RF2_BILLBOARDNOFACECAMERA)) ||
			thing->renderflags2 & RF2_BILLBOARDFACECAMERA))
		{
			RenderStyle.SrcAlpha = STYLEALPHA_One;
			RenderStyle.DestAlpha = STYLEALPHA_Zero;
			hw_styleflags = STYLEHW_Solid;
		}
		else
		{
			RenderStyle.SrcAlpha = STYLEALPHA_Src;
			RenderStyle.DestAlpha = STYLEALPHA_InvSrc;
		}
	}
	if ((texture && texture->GetTranslucency()) || (RenderStyle.Flags & STYLEF_RedIsAlpha) || (modelframe && thing->RenderStyle != DefaultRenderStyle()))
	{
		if (hw_styleflags == STYLEHW_Solid)
		{
			RenderStyle.SrcAlpha = STYLEALPHA_Src;
			RenderStyle.DestAlpha = STYLEALPHA_InvSrc;
		}
		hw_styleflags = STYLEHW_NoAlphaTest;
	}

	if (enhancedvision && gl_enhanced_nightvision)
	{
		if (RenderStyle.BlendOp == STYLEOP_Shadow)
		{
			// enhanced vision makes them more visible!
			trans = 0.5f;
			FRenderStyle rs = RenderStyle;
			RenderStyle = STYLE_Translucent;
			RenderStyle.Flags = rs.Flags;	// Flags must be preserved, at this point it can only be STYLEF_InvertSource
		}
		else if (thing->flags & MF_STEALTH)
		{
			// enhanced vision overcomes stealth!
			if (trans < 0.5f) trans = 0.5f;
		}
	}

	// for sprite shadow, use a translucent stencil renderstyle
	if (isSpriteShadow)
	{
		RenderStyle = STYLE_Stencil;
		ThingColor = MAKEARGB(255, 0, 0, 0);
		// fade shadow progressively as the thing moves higher away from the floor
		if (r_actorspriteshadowfadeheight > 0.0) {
			trans *= clamp(0.0f, float(r_actorspriteshadowalpha - (thingpos.Z - thing->floorz) * (1.0 / r_actorspriteshadowfadeheight)), float(r_actorspriteshadowalpha));
		} else {
			trans *= r_actorspriteshadowalpha;
		}
		hw_styleflags = STYLEHW_NoAlphaTest;
	}

	if (trans == 0.0f) return;

	// end of light calculation

	actor = thing;
	index = thing->SpawnOrder;

	// sprite shadows should have a fixed index of -1 (ensuring they're drawn behind particles which have index 0)
	// sorting should be irrelevant since they're always translucent
	if (isSpriteShadow)
	{
		index = -1;
	}

	particle = nullptr;
	spr = nullptr;

	const bool drawWithXYBillboard = (!(actor->renderflags & RF_FORCEYBILLBOARD)
		&& (actor->renderflags & RF_SPRITETYPEMASK) == RF_FACESPRITE
		&& (gl_billboard_mode == 1 || actor->renderflags & RF_FORCEXYBILLBOARD));


	// no light splitting when:
	// 1. no lightlist
	// 2. any fixed colormap
	// 3. any bright object
	// 4. any with render style shadow (which doesn't use the sector light)
	// 5. anything with render style reverse subtract (light effect is not what would be desired here)
	if (thing->Sector->e->XFloor.lightlist.Size() != 0 && !di->isFullbrightScene() && !fullbright &&
		RenderStyle.BlendOp != STYLEOP_Shadow && RenderStyle.BlendOp != STYLEOP_RevSub)
	{
		lightlist = &thing->Sector->e->XFloor.lightlist;
	}
	else
	{
		lightlist = nullptr;
	}
	PutSprite(di, state, hw_styleflags != STYLEHW_Solid);
	rendered_sprites++;
}


//==========================================================================
//
// 
//
//==========================================================================

void HWSprite::ProcessParticle(HWDrawInfo *di, FRenderState& state, particle_t *particle, sector_t *sector, DVisualThinker *spr)//, int shade, int fakeside)
{
	if (!particle || particle->alpha <= 0)
		return;

	if (spr && !spr->ValidTexture())
		return;

	lightlevel = hw_ClampLight(spr ? spr->GetLightLevel(sector) : sector->GetSpriteLight());
	foglevel = (uint8_t)clamp<short>(sector->lightlevel, 0, 255);

	trans = particle->alpha;
	OverrideShader = (particle->flags & SPF_ALLOWSHADERS) ? -1 : 0;
	modelframe = nullptr;
	texture = nullptr;
	topclip = LARGE_VALUE;
	bottomclip = -LARGE_VALUE;
	index = 0;
	actor = nullptr;
	this->particle = particle;
	this->spr = spr;
	fullbright = particle->flags & SPF_FULLBRIGHT;

	if (di->isFullbrightScene()) 
	{
		Colormap.Clear();
	}
	else if (!(particle->flags & SPF_FULLBRIGHT))
	{
		TArray<lightlist_t> & lightlist=sector->e->XFloor.lightlist;
		double lightbottom;

		Colormap = sector->Colormap;
		for(unsigned int i=0;i<lightlist.Size();i++)
		{
			if (i<lightlist.Size()-1) lightbottom = lightlist[i+1].plane.ZatPoint(particle->Pos);
			else lightbottom = sector->floorplane.ZatPoint(particle->Pos);

			if (lightbottom < particle->Pos.Z)
			{
				lightlevel = hw_ClampLight(*lightlist[i].p_lightlevel);
				Colormap.CopyLight(lightlist[i].extra_colormap);
				break;
			}
		}
		if (di->Level->flags3 & LEVEL3_NOCOLOREDSPRITELIGHTING)
		{
			Colormap.Decolorize();	// ZDoom never applies colored light to particles.
		}
	}
	else
	{
		lightlevel = 255;
		Colormap = sector->Colormap;
		Colormap.ClearColor();
	}

	if(particle->style != STYLE_None)
	{
		RenderStyle = particle->style;
	}
	else
	{
		RenderStyle = STYLE_Translucent;
	}

	ThingColor = particle->color;
	ThingColor.a = 255;
	const auto& vp = di->Viewpoint;

	double timefrac = vp.TicFrac;
	if (paused || (di->Level->isFrozen() && !(particle->flags & SPF_NOTIMEFREEZE)))
		timefrac = 0.;

	
	if (spr && !(spr->flags & VTF_IsParticle))
	{
		AdjustVisualThinker(di, spr, sector);
	}
	else
	{
		bool has_texture = false;
		bool custom_animated_texture = false;
		int particle_style = 0;
		float size = particle->size;
		if (!spr)
		{
			has_texture = particle->texture.isValid();
			custom_animated_texture = (particle->flags & SPF_LOCAL_ANIM) && particle->animData.ok;
			particle_style = has_texture ? 2 : gl_particles_style; // Treat custom texture the same as smooth particles
		}
		else
		{
			size = float(spr->Scale.X);
			const int ptype = spr->GetParticleType();
			particle_style = (ptype != PT_DEFAULT) ? ptype : gl_particles_style;
		}
		// [BB] Load the texture for round or smooth particles
		if (particle_style)
		{
			FTextureID lump;
			if (particle_style == 1)
			{
				lump = TexMan.glPart2;
			}
			else if (particle_style == 2)
			{
				if(custom_animated_texture)
				{
					lump = TexAnim.UpdateStandaloneAnimation(particle->animData, di->Level->maptime + timefrac);
				}
				else if(has_texture)
				{
					lump = particle->texture;
				}
				else
				{
					lump = TexMan.glPart;
				}
			}
			else
			{
				lump.SetNull();
			}

			if (lump.isValid())
			{
				translation = NO_TRANSLATION;

				ul = vt = 0;
				ur = vb = 1;

				texture = TexMan.GetGameTexture(lump, !custom_animated_texture);
			}
		}


		float xvf = (particle->Vel.X) * timefrac;
		float yvf = (particle->Vel.Y) * timefrac;
		float zvf = (particle->Vel.Z) * timefrac;

		offx = 0.f;
		offy = 0.f;

		x = float(particle->Pos.X) + xvf;
		y = float(particle->Pos.Y) + yvf;
		z = float(particle->Pos.Z) + zvf;

		if(particle->flags & SPF_ROLL)
		{
			float rvf = (particle->RollVel) * timefrac;
			Angles.Roll = TAngle<double>::fromDeg(particle->Roll + rvf);
		}
	
		float factor;
		if (particle_style == 1) factor = 1.3f / 7.f;
		else if (particle_style == 2) factor = 2.5f / 7.f;
		else factor = 1 / 7.f;
		float scalefac= size * factor;

		float ps = di->Level->pixelstretch;

		scalefac /= sqrt(ps); // shrink it slightly to account for the stretch

		float viewvecX = vp.ViewVector.X * scalefac * ps;
		float viewvecY = vp.ViewVector.Y * scalefac;

		x1=x+viewvecY;
		x2=x-viewvecY;
		y1=y-viewvecX;
		y2=y+viewvecX;
		z1=z-scalefac;
		z2=z+scalefac;

		depth = (float)((x - vp.Pos.X) * vp.TanCos + (y - vp.Pos.Y) * vp.TanSin);
	
		// [BB] Translucent particles have to be rendered without the alpha test.
		if (particle_style != 2 && trans>=1.0f-FLT_EPSILON) hw_styleflags = STYLEHW_Solid;
		else hw_styleflags = STYLEHW_NoAlphaTest;
	}

	if (sector->e->XFloor.lightlist.Size() != 0 && !di->isFullbrightScene() && !fullbright &&
		RenderStyle.BlendOp != STYLEOP_Shadow && RenderStyle.BlendOp != STYLEOP_RevSub)
	{
		lightlist = &sector->e->XFloor.lightlist;
	}
	else
	{
		lightlist = nullptr;
	}

	PutSprite(di, state, hw_styleflags != STYLEHW_Solid);
	rendered_sprites++;
}

// [MC] VisualThinkers are to be rendered akin to actor sprites. The reason this whole system
// is hitching a ride on particle_t is because of the large number of checks with 
// HWSprite elsewhere in the draw lists.
void HWSprite::AdjustVisualThinker(HWDrawInfo* di, DVisualThinker* spr, sector_t* sector)
{
	translation = spr->Translation;

	const auto& vp = di->Viewpoint;
	double timefrac = vp.TicFrac;

	if (paused || spr->isFrozen())
		timefrac = 0.;

	if (spr->flags & VTF_DontInterpolate)
		timefrac = 0.;

	FVector3 interp = spr->InterpolatedPosition(timefrac);
	x = interp.X;
	y = interp.Y;
	z = interp.Z;

	if(spr->modelClass)
	{
		OverrideShader = -1;

		RenderStyle = spr->PT.style;

		ThingColor = (RenderStyle.Flags & STYLEF_ColorIsFixed) ? spr->PT.color : 0xffffff;
		ThingColor.a = 255;

		OverrideShader = -1;
		trans = spr->PT.alpha;
		hw_styleflags = STYLEHW_Normal;

		if (RenderStyle.BlendOp >= STYLEOP_Fuzz && RenderStyle.BlendOp <= STYLEOP_FuzzOrRevSub)
		{
			RenderStyle.CheckFuzz();
			if (RenderStyle.BlendOp == STYLEOP_Fuzz)
			{
				if (gl_fuzztype != 0 && !(RenderStyle.Flags & STYLEF_InvertSource))
				{
					RenderStyle = LegacyRenderStyles[STYLE_Translucent];
					OverrideShader = SHADER_NoTexture + gl_fuzztype;
					trans = 0.99f;	// trans may not be 1 here
					hw_styleflags = STYLEHW_NoAlphaTest;
				}
				else
				{
					// Without shaders only the standard effect is available.
					RenderStyle.BlendOp = STYLEOP_Shadow;
				}
			}
		}

		if (RenderStyle.Flags & STYLEF_TransSoulsAlpha)
		{
			trans = transsouls;
		}
		else if (RenderStyle.Flags & STYLEF_Alpha1)
		{
			trans = 1.f;
		}

		if (trans >= 1.f - FLT_EPSILON && RenderStyle.BlendOp != STYLEOP_Shadow && (
			(RenderStyle.SrcAlpha == STYLEALPHA_One && RenderStyle.DestAlpha == STYLEALPHA_Zero) ||
			(RenderStyle.SrcAlpha == STYLEALPHA_Src && RenderStyle.DestAlpha == STYLEALPHA_InvSrc)
			))
		{
			// This is a non-translucent sprite (i.e. STYLE_Normal or equivalent)
			trans = 1.f;

			RenderStyle.SrcAlpha = STYLEALPHA_One;
			RenderStyle.DestAlpha = STYLEALPHA_Zero;
			hw_styleflags = STYLEHW_Solid;
		}

		if ((RenderStyle.Flags & STYLEF_RedIsAlpha) || spr->PT.style >= STYLE_Normal)
		{
			if (hw_styleflags == STYLEHW_Solid)
			{
				RenderStyle.SrcAlpha = STYLEALPHA_Src;
				RenderStyle.DestAlpha = STYLEALPHA_InvSrc;
			}
			hw_styleflags = STYLEHW_NoAlphaTest;
		}

		if (di->isFullbrightScene() && di->isStealthVision() && gl_enhanced_nightvision)
		{
			if (RenderStyle.BlendOp == STYLEOP_Shadow)
			{
				// enhanced vision makes them more visible!
				trans = 0.5f;
				FRenderStyle rs = RenderStyle;
				RenderStyle = STYLE_Translucent;
				RenderStyle.Flags = rs.Flags;	// Flags must be preserved, at this point it can only be STYLEF_InvertSource
			}
		}

		modelframe = FindModelFrame(GetDefaultByType(spr->modelClass), spr->modelSprite, spr->modelFrame, false);
		modelframeflags = modelframe ? modelframe->getFlags(nullptr) : 0;

		x1 = x2 = x;
		y1 = y2 = y;
		z1 = z2 = z;
		texture = nullptr;
		
		if (!(spr->flags & VTF_DontInterpolate))
			Angles = DRotator(
						DRotator(DAngle::fromDeg(spr->PrevPitch), DAngle::fromDeg(spr->PrevAngle), DAngle::fromDeg(spr->PrevRoll)),
						DRotator(DAngle::fromDeg(spr->Pitch),DAngle::fromDeg(spr->Angle),  DAngle::fromDeg(spr->PT.Roll)),
						vp.TicFrac);
		else
			Angles = DRotator(DAngle::fromDeg(spr->Pitch), DAngle::fromDeg(spr->Angle), DAngle::fromDeg(spr->PT.Roll));

	}
	else if(spr->PT.texture.isValid())
	{
		bool custom_anim = ((spr->PT.flags & SPF_LOCAL_ANIM) && spr->PT.animData.ok);

		texture = TexMan.GetGameTexture(
				custom_anim
				? TexAnim.UpdateStandaloneAnimation(spr->PT.animData, di->Level->maptime + timefrac)
				: spr->PT.texture, !custom_anim);


		offx = (float)spr->GetOffset(false);
		offy = (float)spr->GetOffset(true);

		if (spr->PT.flags & SPF_ROLL)
			Angles.Roll = TAngle<double>::fromDeg(spr->InterpolatedRoll(timefrac));

		auto& spi = texture->GetSpritePositioning(0);

		vt = spi.GetSpriteVT();
		vb = spi.GetSpriteVB();
		ul = spi.GetSpriteUR();
		ur = spi.GetSpriteUL();

		auto r = spi.GetSpriteRect();

		auto scale = spr->InterpolatedScale(timefrac);

		r.Scale(scale.X, scale.Y);

		if ((spr->PT.flags & SPF_ROLL) && !(spr->PT.flags & SPF_STRETCHPIXELS))
		{
			double ps = di->Level->pixelstretch;
			double mult = 1.0 / sqrt(ps); // shrink slightly
			r.Scale(mult * ps, mult);
		}
		if (spr->flags & VTF_FlipX)
		{
			std::swap(ul,ur);
			r.left = -r.width - r.left;	// mirror the sprite's x-offset
		}
		if (spr->flags & VTF_FlipY)	std::swap(vt,vb);

		float viewvecX = vp.ViewVector.X;
		float viewvecY = vp.ViewVector.Y;
		float rightfac = -r.left;
		float leftfac = rightfac - r.width;

		x1 = x - viewvecY * leftfac;
		x2 = x - viewvecY * rightfac;
		y1 = y + viewvecX * leftfac;
		y2 = y + viewvecX * rightfac;
		z1 = z - r.top;
		z2 = z1 - r.height;

		// [BB] Translucent particles have to be rendered without the alpha test.
		hw_styleflags = STYLEHW_NoAlphaTest;
	}

	depth = (float)((x - vp.Pos.X) * vp.TanCos + (y - vp.Pos.Y) * vp.TanSin);

}

//==========================================================================
//
// 
//
//==========================================================================

void HWDrawInfo::ProcessActorsInPortal(FLinePortalSpan *glport, area_t in_area, FRenderState& state)
{
	TMap<AActor*, bool> processcheck;
	if (glport->validcount == validcount) return;	// only process once per frame
	glport->validcount = validcount;
    const auto &vp = Viewpoint;
	for (auto port : glport->lines)
	{
		line_t *line = port->mOrigin;
		if (line->isLinePortal())	// only crossable ones
		{
			FLinePortal *port2 = port->mDestination->getPortal();
			// process only if the other side links back to this one.
			if (port2 != nullptr && port->mDestination == port2->mOrigin && port->mOrigin == port2->mDestination)
			{
				for (portnode_t *node = port->lineportal_thinglist; node != nullptr; node = node->m_snext)
				{
					AActor *th = node->m_thing;

					// process each actor only once per portal.
					bool *check = processcheck.CheckKey(th);
					if (check && *check) continue;
					processcheck[th] = true;

					DAngle savedangle = th->Angles.Yaw;
					DVector3 savedpos = th->Pos();
					DVector3 newpos = savedpos;
					sector_t fakesector;

					if (!vp.showviewer)
					{
						AActor *viewmaster = th;
						if ((th->flags8 & MF8_MASTERNOSEE) && th->master != nullptr)
						{
							viewmaster = th->master;
						}

						if (viewmaster == vp.camera)
						{
							DVector3 vieworigin = viewmaster->Pos();

							if (fabs(vieworigin.X - vp.ActorPos.X) < 2 && fabs(vieworigin.Y - vp.ActorPos.Y) < 2)
							{
								// Same as the original position
								continue;
							}

							P_TranslatePortalXY(line, vieworigin.X, vieworigin.Y);
							P_TranslatePortalZ(line, vieworigin.Z);

							if (fabs(vieworigin.X - vp.ActorPos.X) < 2 && fabs(vieworigin.Y - vp.ActorPos.Y) < 2)
							{
								// Same as the translated position
								// (This is required for MASTERNOSEE actors with 3D models)
								continue;
							}
						}
					}

					P_TranslatePortalXY(line, newpos.X, newpos.Y);
					P_TranslatePortalZ(line, newpos.Z);
					P_TranslatePortalAngle(line, th->Angles.Yaw);
					th->SetXYZ(newpos);
					th->Prev += newpos - savedpos;

					HWSprite spr;

					// [Nash] draw sprite shadow
					if (R_ShouldDrawSpriteShadow(th))
					{
						spr.Process(this, state, th, hw_FakeFlat(drawctx, th->Sector, in_area, false, &fakesector), in_area, 2, true);
					}

					// This is called from the worker thread and must not alter the fake sector cache.
					spr.Process(this, state, th, hw_FakeFlat(drawctx, th->Sector, in_area, false, &fakesector), in_area, 2);
					th->Angles.Yaw = savedangle;
					th->SetXYZ(savedpos);
					th->Prev -= newpos - savedpos;
				}
			}
		}
	}
}
