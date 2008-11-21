/*
    Copyright (c) 2008 NetAllied Systems GmbH

	This file is part of COLLADAMaya.

    Portions of the code are:
    Copyright (c) 2005-2007 Feeling Software Inc.
    Copyright (c) 2005-2007 Sony Computer Entertainment America
    Copyright (c) 2004-2005 Alias Systems Corp.

    Licensed under the MIT Open Source License,
    for details please see LICENSE file or the website
    http://www.opensource.org/licenses/mit-license.php
*/

#include "COLLADAMayaStableHeaders.h"
#include "COLLADAMayaEffectExporter.h"
#include "COLLADAMayaAnimationExporter.h"
#include "COLLADAMayaSceneGraph.h"
#include "COLLADAMayaDagHelper.h"
#include "COLLADAMayaShaderHelper.h"
#include "COLLADAMayaSyntax.h"
#if MAYA_API_VERSION > 700 
#include "COLLADAMayaHwShaderExporter.h"
#endif

#include "COLLADASWUtils.h"
#include "COLLADASWNode.h"
#include "COLLADASWEffectProfile.h"
#include "COLLADASWExtraTechnique.h"

#include <assert.h>

#include <maya/MItMeshPolygon.h>
#include <maya/MFnLambertShader.h>
#include <maya/MFnMesh.h>
#include <maya/MFnReflectShader.h>
#include <maya/MFnPhongShader.h>
#include <maya/MFnBlinnShader.h>
#ifndef _MPxHwShaderNode
#include <maya/MPxHwShaderNode.h>
#endif // _MPxHwShaderNode

namespace COLLADAMaya
{

    const String EffectExporter::EFFECT_ID_SUFFIX = "-fx";
    const String EffectExporter::COLOR_EFFECT_ID_PREFIX = "ColorEffect";
    const String EffectExporter::TEXCOORD_BASE = "CHANNEL";


    //------------------------------------------------------
    EffectExporter::EffectExporter ( COLLADASW::StreamWriter* _streamWriter, DocumentExporter* _documentExporter )
            : COLLADASW::LibraryEffects ( _streamWriter ),
            mDocumentExporter ( _documentExporter ),
            mTextureExporter ( _documentExporter ),
            mMaterialMap ( NULL )
    {
    }


    //------------------------------------------------------
    const ImageMap* EffectExporter::exportEffects ( MaterialMap* materialMap/*=NULL*/ )
    {
        // Look for the material std::map to export
        if ( materialMap == NULL )
        {
            MaterialExporter* materialExporter = mDocumentExporter->getMaterialExporter();
            mMaterialMap = materialExporter->getExportedMaterialsMap();
        }
        else
        {
            mMaterialMap = materialMap;
        }

        // Iterate through the list of materials and export them
        MaterialMap::iterator materialsIter = mMaterialMap->begin();
        for ( ; materialsIter != mMaterialMap->end(); ++materialsIter )
        {
            MObject* shadingEngine = & ( ( *materialsIter ).second );
            exportEffect ( *shadingEngine );
        }

        closeLibrary();

        // Return the list with the images to export.
        return mTextureExporter.getExportedImageMap();
    }

    //------------------------------------------------------
    void EffectExporter::exportEffectsBySceneGraph()
    {
        SceneGraph* sceneGraph = mDocumentExporter->getSceneGraph();
        SceneElementsList* rootExportNodes = sceneGraph->getExportNodesTree();

        // Export all/selected DAG nodes
        size_t length = rootExportNodes->size();
        for ( size_t i = 0; i < length; ++i )
        {
            SceneElement* sceneElement = ( *rootExportNodes ) [i];

            exportMeshEffects ( sceneElement );
        }
    }

    //------------------------------------------------------
    void EffectExporter::exportMeshEffects ( SceneElement* sceneElement )
    {
        // If we have a external reference, we don't need to export the data here.
        if ( !sceneElement->getIsLocal() ) return;

        // Get the current path
        const MDagPath dagPath = sceneElement->getPath();

        // Check if it is a mesh and an export node
        if ( sceneElement->getType() == SceneElement::MESH &&
             sceneElement->getIsExportNode() )
        {
            MStatus status;
            MFnMesh fnMesh ( dagPath.node(), &status );

            if ( status != MStatus::kSuccess ) return;

            // Find how many shaders are used by this instance of the mesh
            MObjectArray shaders;

            MIntArray shaderIndices;
            unsigned instanceNumber = dagPath.instanceNumber();
            fnMesh.getConnectedShaders ( instanceNumber, shaders, shaderIndices );

            // Find the polygons that correspond to each materials and export them
            uint realShaderCount = ( uint ) shaders.length();
            uint numShaders = ( uint ) std::max ( ( size_t ) 1, ( size_t ) shaders.length() );
            for ( uint shaderPosition = 0; shaderPosition < numShaders; ++shaderPosition )
            {
                if ( shaderPosition < realShaderCount )
                {
                    // Add shader-specific parameters (TexCoords sets).
                    // Add symbolic name for the material used on this polygon set.
                    MObject shadingEngine = shaders[shaderPosition];
                    MObject shader = DagHelper::getSourceNodeConnectedTo ( shadingEngine, ATTR_SURFACE_SHADER );
                    exportEffect ( shader );
                }
            }
        }

        // recursive call for all the child elements
        for ( uint i=0; i<sceneElement->getChildCount(); ++i )
        {
            SceneElement* childElement = sceneElement->getChild ( i );
            exportMeshEffects ( childElement );
        }
    }

    //------------------------------------------------------
    // Add a shading network to this library and return the export Id
    //
    void EffectExporter::exportEffect ( MObject &shader )
    {
        // Find the actual shader node, since this function received shading sets as input
        MStatus status;
        MFnDependencyNode shaderFn ( shader, &status );

        if ( status != MStatus::kSuccess ) return;

        // Get the name of the current material
        String materialName = mDocumentExporter->mayaNameToColladaName ( shaderFn.name(), true );

        // Have we seen this shader before?
        MaterialMap::iterator materialMapIter;
        materialMapIter = ( *mMaterialMap ).find ( materialName );
        if ( materialMapIter == ( *mMaterialMap ).end() )
        {
            // This is a new shading engine
            ( *mMaterialMap ) [materialName] = shader;
        }

        // Check if this effect is already exported
        EffectMap::iterator effectMapIter;
        effectMapIter = mExportedEffectMap.find ( materialName );
        if ( effectMapIter != mExportedEffectMap.end() ) return;

        // Push the shader into the mExportedEffectMap
        mExportedEffectMap[materialName] = &shader;

        // Open a tag for the current effect in the collada document
        String effectId = materialName + EffectExporter::EFFECT_ID_SUFFIX;

        openEffect ( effectId );

        // Add the correct effect for the material
        COLLADASW::EffectProfile effectProfile ( mSW );

        if ( shader.hasFn ( MFn::kLambert ) )
        {
            exportStandardShader ( effectId, &effectProfile, shader );
        }
        else if ( shader.hasFn ( MFn::kPluginHwShaderNode ) && shaderFn.typeName() == COLLADA_FX_SHADER )
        {
            MGlobal::displayError("Export of ColladaFXShader not implemented!");
        }
        else if ( shader.hasFn ( MFn::kPluginHwShaderNode ) && shaderFn.typeName() == COLLADA_FX_PASSES )
        {
            MGlobal::displayError("Export of ColladaFXPasses not implemented!");
        }

#if MAYA_API_VERSION > 700 
        // Custom hardware shaders derived from MPxHardwareShader (the new stuff)
        else if ( shader.hasFn ( MFn::kPluginHwShaderNode ) )
        {
            // Export a cgfx hardware shader node.
            exportHwShaderNode ( effectId, &effectProfile, shader );
        }
#endif

#if MAYA_API_VERSION < 700 || MAYA_API_VERSION == 200800
        // Custom hardware shaders derived from MPxHwShaderNode (the old stuff)
        else if ( shader.hasFn ( MFn::kPluginHardwareShader ) )
        {
            MGlobal::displayError("Export HardwareShader not implemented!");
        }
#endif

        else
        {
            // For the constant shader, you should use the "surface shader" node in Maya
            // But always export some material parameters, even if we don't know this material.
            exportConstantShader ( effectId, &effectProfile, shader );
        }

        // Closes the current effect tag
        closeEffect();
    }

    // ---------------------------------
    void EffectExporter::exportHwShaderNode (
        const String &effectId,
        COLLADASW::EffectProfile *effectProfile,
        MObject shader )
    {
#if MAYA_API_VERSION > 700 
        HwShaderExporter hwShaderExporter ( mDocumentExporter );
        hwShaderExporter.exportPluginHwShaderNode ( effectId, effectProfile, shader );
#endif
    }

    //------------------------------------------------------
    void EffectExporter::exportConstantShader (
        const String &effectId,
        COLLADASW::EffectProfile *effectProfile,
        MObject shader )
    {
        // Create the constant effect
        effectProfile->setShaderType ( COLLADASW::EffectProfile::CONSTANT );

        // Set the constant color/texture

        // Export emission
        MColor outColor;
        DagHelper::getPlugValue ( shader, ATTR_OUT_COLOR, outColor );
        effectProfile->setEmission ( mayaColor2ColorOrTexture ( outColor ) );

        // Get the animation exporter
        AnimationExporter* animationExporter = mDocumentExporter->getAnimationExporter();

        // The target id for the animation
        String targetSid;

        // Get the animation target path
        String targetPath = effectId + "/" + effectProfile->getTechniqueSid() + "/";
        // Build the target sid
        targetSid = targetPath + ATTR_OUT_COLOR;
        // Check, if the parameter is animated
        bool animated = animationExporter->addNodeAnimation ( shader, targetSid, ATTR_OUT_COLOR, kColour );
        // Export out color
        int nextTextureIndex = 0;
        exportTexturedParameter ( effectId, effectProfile, shader,
            ATTR_OUT_COLOR, EffectExporter::EMISSION, nextTextureIndex, animated );

        // Transparent color
        MColor transparentColor;
        DagHelper::getPlugValue ( shader, ATTR_OUT_TRANSPARENCY, transparentColor );
        exportTransparency ( effectId, effectProfile, shader, transparentColor,
            ATTR_OUT_TRANSPARENCY, nextTextureIndex );

        // Writes the current effect profile into the collada document
        effectProfile->openProfile ();
        effectProfile->addProfileElements ();
        effectProfile->closeProfile ();

    }

    //------------------------------------------------------
    void EffectExporter::exportStandardShader (
        const String &effectId,
        COLLADASW::EffectProfile *effectProfile,
        MObject shader,
        bool initialized )
    {
        MFnDependencyNode shaderFn ( shader );
        MFnLambertShader lambertFn ( shader );

        int nextTextureIndex = 0;

        // Add the shader element: <constant> and the <extra><technique profile="MAYA"> elements
        if ( shader.hasFn ( MFn::kPhong ) )
            effectProfile->setShaderType ( COLLADASW::EffectProfile::PHONG );
        else if ( shader.hasFn ( MFn::kBlinn ) )
            effectProfile->setShaderType ( COLLADASW::EffectProfile::BLINN );
        else effectProfile->setShaderType ( COLLADASW::EffectProfile::LAMBERT );

        // Get the animation exporter and the animation target path
        AnimationExporter* animationExporter = mDocumentExporter->getAnimationExporter();
        // Get the animation target path
        String targetPath = effectId + "/" + effectProfile->getTechniqueSid() + "/";
        // The target id for the animation
        String targetSid;
        // Flag is true for animated parameters.
        bool animated = false;

        // Emission color / Incandescence
        targetSid = targetPath + COLLADASW::CSWC::CSW_ELEMENT_EMISSION;
        animated = animationExporter->addNodeAnimation ( shader, targetSid, ATTR_INCANDESCENCE, kColour );
        effectProfile->setEmission ( mayaColor2ColorOrTexture ( lambertFn.incandescence() ), animated );
        exportTexturedParameter ( effectId, effectProfile, shader,
            ATTR_INCANDESCENCE, EffectExporter::EMISSION, nextTextureIndex, animated );

        // Ambient color
        targetSid = targetPath + ATTR_AMBIENT_COLOR;
        animated = animationExporter->addNodeAnimation ( shader, targetSid, ATTR_AMBIENT_COLOR, kColour );
        effectProfile->setAmbient ( mayaColor2ColorOrTexture ( lambertFn.ambientColor() ), animated );
        exportTexturedParameter ( effectId, effectProfile, shader,
            ATTR_AMBIENT_COLOR, EffectExporter::AMBIENT, nextTextureIndex, animated );

        // Diffuse color
        targetSid = targetPath + ATTR_COLOR;
        ConversionFunctor* conversion = new ConversionScaleFunctor ( lambertFn.diffuseCoeff() );
        animated = animationExporter->addNodeAnimation ( shader, targetSid, ATTR_COLOR, kColour, EMPTY_PARAMETER, false, -1, false, conversion );
        effectProfile->setDiffuse ( mayaColor2ColorOrTexture ( lambertFn.color(), lambertFn.diffuseCoeff() ), animated );
        exportTexturedParameter ( effectId, effectProfile, shader,
            ATTR_COLOR, EffectExporter::DIFFUSE, nextTextureIndex, animated );

        // Transparent color
        exportTransparency ( effectId, effectProfile, shader, lambertFn.transparency(),
            ATTR_TRANSPARENCY, nextTextureIndex );

        float coeff = lambertFn.translucenceCoeff();
        effectProfile->setTransparency ( 1.0f );

        // Bump textures
        exportTexturedParameter ( effectId, effectProfile, shader,
            ATTR_NORMAL_CAMERA, EffectExporter::BUMP, nextTextureIndex );

        if ( shader.hasFn ( MFn::kReflect ) ) // includes Phong and Blinn
        {
            MFnReflectShader reflectFn ( shader );

            // Specular color
            if ( effectProfile->getShaderType() != COLLADASW::EffectProfile::LAMBERT )
            {
                targetSid = targetPath + ATTR_SPECULAR_COLOR;
                animated = animationExporter->addNodeAnimation ( shader, targetSid, ATTR_SPECULAR_COLOR, kColour );
                effectProfile->setSpecular ( mayaColor2ColorOrTexture ( reflectFn.specularColor() ), animated );
                exportTexturedParameter ( effectId, effectProfile, shader,
                    ATTR_SPECULAR_COLOR, EffectExporter::SPECULAR, nextTextureIndex, animated );
            }

            // Reflected color
            targetSid = targetPath + ATTR_REFLECTED_COLOR;
            animated = animationExporter->addNodeAnimation ( shader, targetSid, ATTR_REFLECTED_COLOR, kColour );
            effectProfile->setReflective ( mayaColor2ColorOrTexture ( reflectFn.reflectedColor() ), animated );
            exportTexturedParameter ( effectId, effectProfile, shader,
                ATTR_REFLECTED_COLOR, EffectExporter::REFLECTION, nextTextureIndex, animated );

            // Reflectivity factor
            targetSid = targetPath + ATTR_REFLECTIVITY;
            animated = animationExporter->addNodeAnimation ( shader, targetSid, ATTR_REFLECTIVITY, kSingle );
            effectProfile->setReflectivity ( reflectFn.reflectivity(), animated );
        }

        // index of refraction
        bool refractive;
        DagHelper::getPlugValue ( shader, ATTR_REFRACTIONS, refractive );
        if ( refractive )
        {
            targetSid = targetPath + COLLADASW::CSWC::CSW_ELEMENT_INDEX_OF_REFRACTION;
            animated = animationExporter->addNodeAnimation ( shader, targetSid, ATTR_REFRACTIVE_INDEX, kSingle );
            effectProfile->setIndexOfRefraction ( lambertFn.refractiveIndex(), animated );
        }

        // Phong and Blinn's specular factor
        if ( shader.hasFn ( MFn::kPhong ) )
        {
            MFnPhongShader phongFn ( shader );
            targetSid = targetPath + ATTR_COSINE_POWER;
            animated = animationExporter->addNodeAnimation ( shader, targetSid, ATTR_COSINE_POWER, kSingle );
            effectProfile->setShininess ( phongFn.cosPower(), animated );
        }
        else if ( shader.hasFn ( MFn::kBlinn ) )
        {
#ifdef BLINN_EXPONENT_MODEL
            MFnBlinnShader blinnFn ( shader );
            BlinnEccentricityToShininess* converter = new BlinnEccentricityToShininess();
            double shininess = ( *converter ) ( blinnFn.eccentricity() );
            targetSid = targetPath + ATTR_ECCENTRICITY;
            animated = animationExporter->addNodeAnimation ( shader, targetSid, ATTR_ECCENTRICITY, kSingle, EMPTY_PARAMETER, -1, false, converter );
            effectProfile->setShininess ( shininess, animated );
            delete converter;
#else
            MFnBlinnShader blinnFn ( shader );
            targetSid = targetPath + ATTR_ECCENTRICITY;
            animated = animationExporter->addNodeAnimation ( shader, targetSid, ATTR_ECCENTRICITY, kSingle );
            effectProfile->setShininess ( blinnFn.eccentricity(), animated );
#endif // BLINN_EXPONENT_MODEL
        }

        // Writes the current effect profile into the collada document
        effectProfile->openProfile ();
        effectProfile->addProfileElements ();
        effectProfile->closeProfile ();
    }


    //---------------------------------------------------------------
    MObject EffectExporter::exportTexturedParameter(
        const String& effectId,
        COLLADASW::EffectProfile* effectProfile,
        const MObject& node,
        const char* attributeName,
        EffectExporter::Channel channel,
        int& nextTextureIndex,
        bool animated )
    {
        // Find any textures connected to a material attribute and create the
        // associated texture elements.

        // Retrieve all the file textures with the blend modes, if exist.
        MObjectArray fileTextures;
        MIntArray blendModes;
        getShaderTextures ( node, attributeName, fileTextures, blendModes );

        // What the hell??? Collada tells me, that there can be only
        // one texture for every related shader element!!!
        uint fileTextureCount = fileTextures.length();
        for ( uint i = 0; i < fileTextureCount; ++i )
        {
            // Verify that the texture is linked to a filename: COLLADA doesn't like empty file texture nodes.
            MFnDependencyNode nodeFn ( fileTextures[i] );
            MPlug filenamePlug = nodeFn.findPlug ( ATTR_FILE_TEXTURE_NAME );
            MString filename;
            filenamePlug.getValue ( filename );
            if ( filename.length() == 0 ) continue;

            // Create the texture linking object.
            MObject fileTexture = fileTextures[i];
            int blendMode = blendModes[i];
            String channelSemantic = TEXCOORD_BASE + COLLADASW::Utils::toString ( channel );

            // Get the animation target path
            String targetPath = effectId + "/" + effectProfile->getTechniqueSid() + "/";

            COLLADASW::Texture colladaTexture;
            mTextureExporter.exportTexture ( &colladaTexture,
                                             channelSemantic,
                                             fileTextures[i],
                                             blendModes[i],
                                             targetPath );
            nextTextureIndex++;

            // Special case for bump maps: export the bump height in the "amount" texture parameter.
            // Exists currently within the ColladaMax profile.
            if ( channel == EffectExporter::BUMP )
            {
                MObject bumpNode = DagHelper::getNodeConnectedTo ( node, attributeName );
                if ( !bumpNode.isNull() && ( bumpNode.hasFn ( MFn::kBump ) || bumpNode.hasFn ( MFn::kBump3d ) ) )
                {
                    // Get the animation exporter
                    AnimationExporter* animationExporter = mDocumentExporter->getAnimationExporter();

                    // The target id for the animation
                    String targetSid = targetPath + MAX_AMOUNT_TEXTURE_PARAMETER;
                    bool animated = animationExporter->addNodeAnimation ( bumpNode, targetSid, ATTR_BUMP_DEPTH, kSingle );

                    float amount = 1.0f;
                    MFnDependencyNode ( bumpNode ).findPlug ( ATTR_BUMP_DEPTH ).getValue ( amount );
                    String paramSid = ""; if ( animated ) paramSid = MAX_AMOUNT_TEXTURE_PARAMETER;
                    colladaTexture.addExtraTechniqueParameter ( MAX_PROFILE, MAX_AMOUNT_TEXTURE_PARAMETER, amount, paramSid );

                    int interp = 0;
                    MFnDependencyNode ( bumpNode ).findPlug ( ATTR_BUMP_INTERP ).getValue ( interp );
                    colladaTexture.addExtraTechniqueParameter ( MAX_PROFILE, MAX_BUMP_INTERP_TEXTURE_PARAMETER, interp );
                }
            }

            // Change the color values to textures
            switch ( channel )
            {
                // TODO
            case AMBIENT:
                effectProfile->setAmbient ( COLLADASW::ColorOrTexture ( colladaTexture ), animated );
                break;
            case BUMP:
            {
                // Set the profile name and the child element name to the texture.
                // Then we can add it as the extra technique texture.
                colladaTexture.setProfileName( COLLADASW::CSWC::CSW_PROFILE_COLLADA );
                colladaTexture.setChildElementName( MAYA_BUMP_PARAMETER );
                effectProfile->setExtraTechniqueColorOrTexture( COLLADASW::ColorOrTexture ( colladaTexture ), MAYA_BUMP_PARAMETER );
                break;
            }
            case DIFFUSE:
                effectProfile->setDiffuse ( COLLADASW::ColorOrTexture ( colladaTexture ), animated );
                break;
            //  case DISPLACEMENT: displacementTextures.push_back(COLLADASW::ColorOrTexture(colladaTexture)); break;
            case EMISSION:
                effectProfile->setEmission ( COLLADASW::ColorOrTexture ( colladaTexture ), animated );
                break;
                //  case FILTER: filterTextures.push_back(COLLADASW::ColorOrTexture(colladaTexture)); break;
            case REFLECTION:
                effectProfile->setReflective ( COLLADASW::ColorOrTexture ( colladaTexture ), animated );
                break;
                //  case REFRACTION: refractionTextures.push_back(COLLADASW::ColorOrTexture(colladaTexture)); break;
                //  case SHININESS: shininessTextures.push_back(COLLADASW::ColorOrTexture(colladaTexture)); break;
            case SPECULAR:
                effectProfile->setSpecular ( COLLADASW::ColorOrTexture ( colladaTexture ), animated );
                break;
                //  case SPECULAR_LEVEL: specularFactorTextures.push_back(COLLADASW::ColorOrTexture(colladaTexture)); break;
            case TRANSPARENt:
                effectProfile->setTransparent ( COLLADASW::ColorOrTexture ( colladaTexture ), animated );
                break;
            default:
                break;
            }
        }

        return ( fileTextures.length() > 0 ) ? fileTextures[0] : MObject::kNullObj;
    }

    //---------------------------------------------------------------
    // Retrieve any texture (file or layered) associated with a material attribute
    //
    void EffectExporter::getShaderTextures (
        const MObject& shader,
        const char* attributeName,
        MObjectArray& textures,
        MIntArray& blendModes )
    {
        MObject texture = DagHelper::getSourceNodeConnectedTo ( shader, attributeName );

        while ( texture != MObject::kNullObj &&
                !texture.hasFn ( MFn::kLayeredTexture ) &&
                !texture.hasFn ( MFn::kFileTexture ) )
        {
            // Bypass the bump and projection nodes
            if ( texture.hasFn ( MFn::kBump ) || texture.hasFn ( MFn::kBump3d ) )
                texture = DagHelper::getSourceNodeConnectedTo ( texture, ATTR_BUMP_VALUE );
            else if ( texture.hasFn ( MFn::kProjection ) )
                texture = DagHelper::getSourceNodeConnectedTo ( texture, ATTR_IMAGE );

            else break;
        }

        // Verify that we have a supported texture type: file or layered.
        bool isFileTexture = texture.hasFn ( MFn::kFileTexture );
        bool isLayeredTexture = texture.hasFn ( MFn::kLayeredTexture );
        if ( !isFileTexture && !isLayeredTexture ) return;

        // Return the textures and blend modes
        if ( isFileTexture )
        {
            textures.append ( texture );
            blendModes.append ( 0 ); // 0 -> No blending
        }
        else if ( isLayeredTexture )
        {
            ShaderHelper::getLayeredTextures ( texture, textures, blendModes );
        }
    }

    //---------------------------------------------------------------
    void EffectExporter::blendColor ( COLLADASW::ColorOrTexture &colorOrTexture,
                                      COLLADASW::Color blendColor,
                                      double ammount )
    {
        assert ( colorOrTexture.isColor() );

        COLLADASW::Color& color = colorOrTexture.getColor();

        color.set ( color.getRed() + ( blendColor.getRed()-color.getRed() ) * ammount,
                    color.getGreen() + ( blendColor.getGreen()-color.getGreen() ) * ammount,
                    color.getBlue() + ( blendColor.getBlue()-color.getBlue() ) * ammount,
                    color.getAlpha() );
    }

    //---------------------------------------------------------------
    COLLADASW::ColorOrTexture EffectExporter::mayaColor2ColorOrTexture ( const MColor &color, double scale )
    {
        return COLLADASW::ColorOrTexture ( COLLADASW::Color ( color.r * scale, color.g * scale, color.b * scale, color.a ) );
    }

    //---------------------------------------------------------------
    void EffectExporter::exportTransparency(
        const String& effectId,
        COLLADASW::EffectProfile* effectProfile,
        MObject shadingNetwork,
        const MColor& transparentColor,
        const char* attributeName,
        int& nextTextureIndex )
    {
        // Get the animation target path
        String targetPath = effectId + "/" + effectProfile->getTechniqueSid() + "/";

        // Get the animation exporter
        AnimationExporter* animationExporter = mDocumentExporter->getAnimationExporter();

        // Flag, if the current attribute is animated
        bool animated = false;

        // Build the target sid and export animation
        String targetSubId = targetPath + attributeName;
        animated = animationExporter->addNodeAnimation ( shadingNetwork, targetSubId, attributeName, kColour );
        // Set the transparent color or texture
        effectProfile->setTransparent ( mayaColor2ColorOrTexture ( transparentColor ), animated );

        MObject transparentTextureNode =
            exportTexturedParameter ( effectId, effectProfile, shadingNetwork,
                attributeName, EffectExporter::TRANSPARENt, nextTextureIndex, animated );

        // For the 'opaque' attribute, check the plug's name, that's connected to
        // the shader's 'transparency' plug.
        MPlug connectedPlug;
        if ( !transparentTextureNode.isNull() )
        {
            // DO NOTE: We're missing 2 transparency mode that were wrongly deemed useless in COLLADA 1.4.1.
            // Until then, we cannot use the 'invert' attribute or correctly export the transparency mode.
            MFnDependencyNode texture2DFn ( transparentTextureNode );
            bool isInverted = false;
            texture2DFn.findPlug ( ATTR_INVERT ).getValue ( isInverted );

            DagHelper::getPlugConnectedTo ( shadingNetwork, attributeName, connectedPlug );
            String partialName = connectedPlug.partialName().asChar();

            if ( connectedPlug.partialName() == ATTR_OUT_COLOR )
                effectProfile->setOpaque ( COLLADASW::EffectProfile::RGB_ZERO ); // should be RGB_ONE.
            else if ( connectedPlug.partialName() == ATTR_OUT_TRANSPARENCY )
                effectProfile->setOpaque ( COLLADASW::EffectProfile::A_ONE );
            else if ( connectedPlug.partialName() == ATTR_OUT_ALPHA )
                effectProfile->setOpaque ( COLLADASW::EffectProfile::A_ONE ); // valid?

            if ( effectProfile->getOpaque() == COLLADASW::EffectProfile::A_ONE )
            {
                // Get the animation cache
                AnimationSampleCache* animationCache = mDocumentExporter->getAnimationCache();

                // Export any animation on the alpha gain/alpha offset
                if ( AnimationHelper::isAnimated ( animationCache, transparentTextureNode, ATTR_ALPHA_GAIN ) )
                {
                    // TODO Test
                    String targetSubId = targetPath + ATTR_ALPHA_GAIN;
                    animationExporter->addNodeAnimation ( transparentTextureNode, targetSubId, ATTR_ALPHA_GAIN, kSingle );
                }
                else
                {
                    // TODO Test
                    String targetSubId = targetPath + ATTR_ALPHA_OFFSET;
                    animationExporter->addNodeAnimation ( transparentTextureNode, targetSubId, ATTR_ALPHA_OFFSET, kSingle, EMPTY_PARAMETER, false, -1, false, new ConversionOffsetFunctor ( 1.0f ) );
                }
            }
        }
        else
        {
            effectProfile->setOpaque ( COLLADASW::EffectProfile::RGB_ZERO ); // correctly RGB_zero.
        }
    }

}
