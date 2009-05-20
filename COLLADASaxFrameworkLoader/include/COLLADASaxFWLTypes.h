/*
Copyright (c) 2008-2009 NetAllied Systems GmbH

This file is part of COLLADASaxFrameworkLoader.

Licensed under the MIT Open Source License, 
for details please see LICENSE file or the website
http://www.opensource.org/licenses/mit-license.php
*/

#ifndef __COLLADASAXFWL_TYPES_H__
#define __COLLADASAXFWL_TYPES_H__

#include "COLLADASaxFWLPrerequisites.h"

#include "COLLADAFWUniqueId.h"
#include "COLLADAFWAnimationList.h"
#include "COLLADAFWTypes.h"

#include <list>

namespace COLLADAFW
{
	class Node;
}

namespace COLLADASaxFWL
{

	struct AnimationInfo
	{
		static const AnimationInfo INVALID;
		/** The unique id of the animation*/
		COLLADAFW::UniqueId uniqueId;

		/** The class of the animation.*/
		COLLADAFW::AnimationList::AnimationClass animationClass;
	};

	typedef std::list<String> StringList;

	typedef std::list<COLLADABU::URI> URIList;

	static StringList EMPTY_STRING_LIST = StringList();


#ifdef COLLADASAXFWL_REAL_IS_FLOAT
	typedef COLLADAFW::FloatArray RealArray;
#else
	typedef COLLADAFW::DoubleArray RealArray;
#endif

	typedef std::vector<COLLADAFW::Node*> NodeList;



} // namespace COLLADASAXFWL

#endif // __COLLADASAXFWL_TYPES_H__