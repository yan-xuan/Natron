/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <http://www.natron.fr/>,
 * Copyright (C) 2016 INRIA and Alexandre Gauthier-Foichat
 *
 * Natron is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Natron is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Natron.  If not, see <http://www.gnu.org/licenses/gpl-2.0.html>
 * ***** END LICENSE BLOCK ***** */

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Image.h"

#include <algorithm> // min, max
#include <cassert>
#include <cstring> // for std::memcpy, std::memset
#include <stdexcept>

#include <QtCore/QDebug>

#include "Engine/AppManager.h"
#include "Engine/Cache.h"
#include "Engine/CacheEntryBase.h"
#include "Engine/ViewIdx.h"
#include "Engine/GPUContextPool.h"
#include "Engine/OSGLContext.h"
#include "Engine/OSGLFunctions.h"

NATRON_NAMESPACE_ENTER;

struct MonoChannelTile
{
    // A pointer to the internal storage
    MemoryBufferedCacheEntryBasePtr buffer;
    
    // Used when working with the cache
    // to ensure a single thread computed this tile
    CacheEntryLockerPtr entryLocker;

    // Was this found in the cache ?
    bool isCached;
};

struct ImageTile
{
    // Each ImageTile internally holds a pointer to a mono-channel tile in the cache
    std::vector<MonoChannelTile> perChannelTile;

};

struct ImagePrivate
{
    RectI bounds;

    std::vector<ImageTile> tiles;

    // Controls the cache access for the image
    Image::CacheAccessModeEnum cachePolicy;

    ImagePrivate()
    : bounds()
    , tiles()
    , cachePolicy(Image::eCacheAccessModeEnumNone)
    {

    }
};

Image::Image()
: _imp(new ImagePrivate())
{

}

Image::~Image()
{

}


void
Image::initializeStorage(const Image::InitStorageArgs& args)
{
    CachePtr cache = appPTR->getCache();
    assert(cache);

    // Should be initialized once!
    assert(_imp->tiles.empty());
    if (!_imp->tiles.empty()) {
        return;
    }

    _imp->bounds = args.bounds;
    _imp->cachePolicy = args.cachePolicy;

    int tileSize = cache->getTileSizeBytes();

    int nTilesHeight,nTilesWidth;
    if (args.cachePolicy == eCacheAccessModeEnumNone || args.storage == eStorageModeGLTex) {

        // When not using the cache or a GL texture use a single tile
        nTilesHeight = 1;
        nTilesWidth = 1;
    } else {
        nTilesHeight = std::ceil(_imp->bounds.height() / tileSize) * tileSize;
        nTilesWidth = std::ceil(_imp->bounds.width() / tileSize) * tileSize;
    }

    int nTiles = nTilesWidth * nTilesHeight;
    _imp->tiles.resize(nTiles);

    // Initialize each tile
    int tx = 0, ty = 0;
    for (int tile_i = 0; tile_i < nTiles; ++tile_i) {

        ImageTile& tile = _imp->tiles[tile_i];

        const std::string& layerName = args.layer.getLayerName();

        int nBuffers;
        if (args.storage == eStorageModeGLTex) {
            // GL textures are always RGBA so use 1 texture
            nBuffers = 1;
        } else {
            // As many buffer as there are components
            nBuffers = args.layer.getNumComponents();
        }
        tile.perChannelTile.resize(nBuffers);

        for (int c = 0; c < nBuffers; ++c) {

            MonoChannelTile& thisChannelTile = tile.perChannelTile[c];

            std::string channelName;
            if (args.storage == eStorageModeGLTex) {
                channelName = layerName;
            } else {
                channelName = layerName + "." + args.layer.getComponentsNames()[c];
            }

            ImageTileKeyPtr key(new ImageTileKey(args.nodeHashKey,
                                                 channelName,
                                                 args.mipMapLevel,
                                                 args.isDraft,
                                                 args.bitdepth,
                                                 tx,
                                                 ty));
            {
                CacheEntryBasePtr cacheEntry;
                CacheEntryLockerPtr entryLocker;
                
                // If the entry wants to be cached but we don't want to read from the cache
                // we must remove from the cache any entry that already exists at the given hash.
                if (args.cachePolicy == eCacheAccessModeEnumWriteOnly) {
                    bool isCached = cache->get(key, &cacheEntry, &entryLocker);
                    if (isCached) {
                        assert(cacheEntry);
                        cache->removeEntry(cacheEntry);
                    }
                }
                
                // Look in the cache
                bool isCached = false;
                if (args.cachePolicy == eCacheAccessModeReadWrite || args.cachePolicy == eCacheAccessModeEnumWriteOnly) {
                    isCached = cache->get(key, &cacheEntry, &entryLocker);
                    if (isCached) {
                        // Found in cache, check if the key is really equal to this key
                        if (key->equals(*cacheEntry->getKey())) {
                            MemoryBufferedCacheEntryBasePtr isBufferedEntry = boost::dynamic_pointer_cast<MemoryBufferedCacheEntryBase>(cacheEntry);
                            assert(isBufferedEntry);
                            thisChannelTile.buffer = isBufferedEntry;
                            thisChannelTile.isCached = true;
                        }
                    }
                }
                
                if (!isCached) {
                    thisChannelTile.isCached = false;
                
                    // Make the hash locker live as long as this image will live
                    thisChannelTile.entryLocker = entryLocker;
#pragma message WARN("more wip")
                    boost::shared_ptr<AllocateMemoryArgs> allocArgs;
                    MemoryBufferedCacheEntryBasePtr entryBuffer;
                    // Allocate a new entry
                    switch (args.storage) {
                        case eStorageModeDisk: {
                            MemoryMappedCacheEntryPtr buffer(new MemoryMappedCacheEntry(cache));
                            entryBuffer = buffer;
                            boost::shared_ptr<MMAPAllocateMemoryArgs> a(new MMAPAllocateMemoryArgs());
                            a->bitDepth = args.bitdepth;
                            allocArgs = a;
                        }   break;
                        case eStorageModeGLTex: {
                            GLCacheEntryPtr buffer(new GLCacheEntry(cache));
                            entryBuffer = buffer;
                            boost::shared_ptr<GLAllocateMemoryArgs> a(new GLAllocateMemoryArgs());
                            allocArgs = a;
                        }   break;
                        case eStorageModeRAM: {
                            RAMCacheEntryPtr buffer(new RAMCacheEntry(cache));
                            entryBuffer = buffer;
                            boost::shared_ptr<RAMAllocateMemoryArgs> a(new RAMAllocateMemoryArgs());
                            a->bitDepth = args.bitdepth;
                            allocArgs = a;
                        }   break;
                        case eStorageModeNone:
                            assert(false);
                            throw std::bad_alloc();
                            break;
                    }
                    assert(allocArgs && entryBuffer);
                    
                    // This may throw a std::bad_alloc
                    entryBuffer->allocateMemory(*allocArgs);
                }
                
            }
        } // for each channel

        // Increment tile coords
        if (tx == nTilesWidth - 1) {
            tx = 0;
            ++ty;
        } else {
            ++tx;
        }
    } // for each tile

} // initializeStorage


void
Image::setBitmapDirtyZone(const RectI& zone)
{
    QWriteLocker k(&_entryLock);

    _bitmap.setDirtyZone(zone);
}

ImageParamsPtr
Image::makeParams(const RectD & rod,
                  const double par,
                  unsigned int mipMapLevel,
                  const ImageComponents& components,
                  ImageBitDepthEnum bitdepth,
                  ImagePremultiplicationEnum premult,
                  ImageFieldingOrderEnum fielding,
                  const OSGLContextPtr& context,
                  StorageModeEnum storage,
                  U32 textureTarget)
{
    RectI bounds;

    rod.toPixelEnclosing(mipMapLevel, par, &bounds);

    return ImageParamsPtr( new ImageParams(rod,
                                           par,
                                           mipMapLevel,
                                           bounds,
                                           bitdepth,
                                           fielding,
                                           premult,
                                           components,
                                           context,
                                           storage,
                                           textureTarget) );
}

ImageParamsPtr
Image::makeParams(const RectD & rod,    // the image rod in canonical coordinates
                  const RectI& bounds,
                  const double par,
                  unsigned int mipMapLevel,
                  const ImageComponents& components,
                  ImageBitDepthEnum bitdepth,
                  ImagePremultiplicationEnum premult,
                  ImageFieldingOrderEnum fielding,
                  const OSGLContextPtr& context,
                  StorageModeEnum storage,
                  U32 textureTarget)
{
#ifdef DEBUG
    RectI pixelRod;
    rod.toPixelEnclosing(mipMapLevel, par, &pixelRod);
    assert( bounds.left() >= pixelRod.left() && bounds.right() <= pixelRod.right() &&
           bounds.bottom() >= pixelRod.bottom() && bounds.top() <= pixelRod.top() );
#endif

    return ImageParamsPtr( new ImageParams(rod,
                                           par,
                                           mipMapLevel,
                                           bounds,
                                           bitdepth,
                                           fielding,
                                           premult,
                                           components,
                                           context,
                                           storage,
                                           textureTarget) );
}

// code proofread and fixed by @devernay on 8/8/2014
template<typename PIX>
void
Image::pasteFromForDepth(const Image & srcImg,
                         const RectI & srcRoi,
                         bool copyBitmap,
                         bool takeSrcLock)
{
    ///Cannot copy images with different bit depth, this is not the purpose of this function.
    ///@see convert
    assert( getBitDepth() == srcImg.getBitDepth() );
    assert( (getBitDepth() == eImageBitDepthByte && sizeof(PIX) == 1) || (getBitDepth() == eImageBitDepthShort && sizeof(PIX) == 2) || (getBitDepth() == eImageBitDepthFloat && sizeof(PIX) == 4) );
    // NOTE: before removing the following asserts, please explain why an empty image may happen

    QWriteLocker k(&_entryLock);
    boost::shared_ptr<QReadLocker> k2;
    if (takeSrcLock) {
        k2.reset( new QReadLocker(&srcImg._entryLock) );
    }

    const RectI & bounds = _bounds;
    const RectI & srcBounds = srcImg._bounds;

    assert( !bounds.isNull() );
    assert( !srcBounds.isNull() );

    // only copy the intersection of roi, bounds and otherBounds
    RectI roi = srcRoi;
    bool doInteresect = roi.intersect(bounds, &roi);
    if (!doInteresect) {
        // no intersection between roi and the bounds of this image
        return;
    }
    doInteresect = roi.intersect(srcBounds, &roi);
    if (!doInteresect) {
        // no intersection between roi and the bounds of the other image
        return;
    }

    assert( getComponents() == srcImg.getComponents() );

    if (copyBitmap && _useBitmap) {
        copyBitmapPortion(roi, srcImg);
    }
    // now we're safe: both images contain the area in roi

    int srcRowElements = _nbComponents * srcBounds.width();
    int dstRowElements = _nbComponents * bounds.width();
    const PIX* src = (const PIX*)srcImg.pixelAt(roi.x1, roi.y1);
    PIX* dst = (PIX*)pixelAt(roi.x1, roi.y1);

    assert(src && dst);

    for (int y = roi.y1; y < roi.y2;
         ++y,
         src += srcRowElements,
         dst += dstRowElements) {
        std::memcpy(dst, src, roi.width() * sizeof(PIX) * _nbComponents);
    }
} // Image::pasteFromForDepth

void
Image::setRoD(const RectD& rod)
{
    QWriteLocker k(&_entryLock);

    _rod = rod;
    _params->setRoD(rod);
}



template <typename GL>
static void
pasteFromGL(const Image & src,
            Image* dst,
            const RectI & srcRoi,
            bool /*copyBitmap*/,
            const OSGLContextPtr& glContext,
            const RectI& srcBounds,
            const RectI& dstBounds,
            StorageModeEnum thisStorage,
            StorageModeEnum otherStorage,
            int target)
{

    int texID = dst->getGLTextureID();
    if ( (thisStorage == eStorageModeGLTex) && (otherStorage == eStorageModeGLTex) ) {
        // OpenGL texture to OpenGL texture

        GLuint fboID = glContext->getOrCreateFBOId();
        GL::Disable(GL_SCISSOR_TEST);
        GL::BindFramebuffer(GL_FRAMEBUFFER, fboID);
        GL::Enable(target);
        GL::ActiveTexture(GL_TEXTURE0);

        GL::BindTexture( target, texID );

        GL::TexParameteri (target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        GL::TexParameteri (target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

        GL::TexParameteri (target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        GL::TexParameteri (target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);


        GL::FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, target, texID, 0 /*LoD*/);
        glCheckFramebufferError(GL);
        GL::BindTexture( target, src.getGLTextureID() );

        GL::TexParameteri (target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        GL::TexParameteri (target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

        GL::TexParameteri (target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        GL::TexParameteri (target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        GLShaderBasePtr shader = glContext->getOrCreateCopyTexShader();
        assert(shader);
        shader->bind();
        shader->setUniform("srcTex", 0);

        Image::applyTextureMapping<GL>(srcBounds, dstBounds, srcRoi);

        shader->unbind();
        GL::BindTexture(target, 0);
        
        glCheckError(GL);
    } else if ( (thisStorage == eStorageModeGLTex) && (otherStorage != eStorageModeGLTex) ) {
        // RAM image to OpenGL texture

        // only copy the intersection of roi, bounds and otherBounds
        RectI roi = srcRoi;
        bool doInteresect = roi.intersect(dstBounds, &roi);
        if (!doInteresect) {
            // no intersection between roi and the bounds of this image
            return;
        }
        doInteresect = roi.intersect(srcBounds, &roi);
        if (!doInteresect) {
            // no intersection between roi and the bounds of the other image
            return;
        }
        GLuint pboID = glContext->getOrCreatePBOId();
        GL::Enable(target);

        // bind PBO to update texture source
        GL::BindBufferARB(GL_PIXEL_UNPACK_BUFFER_ARB, pboID);

        std::size_t dataSize = roi.area() * 4 * src.getParams()->getStorageInfo().dataTypeSize;

        // Note that glMapBufferARB() causes sync issue.
        // If GPU is working with this buffer, glMapBufferARB() will wait(stall)
        // until GPU to finish its job. To avoid waiting (idle), you can call
        // first glBufferDataARB() with NULL pointer before glMapBufferARB().
        // If you do that, the previous data in PBO will be discarded and
        // glMapBufferARB() returns a new allocated pointer immediately
        // even if GPU is still working with the previous data.
        GL::BufferDataARB(GL_PIXEL_UNPACK_BUFFER_ARB, dataSize, 0, GL_DYNAMIC_DRAW_ARB);

        // map the buffer object into client's memory
        void* gpuData = GL::MapBufferARB(GL_PIXEL_UNPACK_BUFFER_ARB, GL_WRITE_ONLY_ARB);
        assert(gpuData);
        if (gpuData) {
            // update data directly on the mapped buffer
            ImagePtr tmpImg( new Image( ImageComponents::getRGBAComponents(), src.getRoD(), roi, 0, src.getPixelAspectRatio(), src.getBitDepth(), src.getPremultiplication(), src.getFieldingOrder(), false, eStorageModeRAM, OSGLContextPtr(), GL_TEXTURE_2D, true) );
            tmpImg->pasteFrom(src, roi);

            Image::ReadAccess racc(tmpImg ? tmpImg.get() : dst);
            const unsigned char* srcdata = racc.pixelAt(roi.x1, roi.y1);
            assert(srcdata);

            memcpy(gpuData, srcdata, dataSize);

            GLboolean result = GL::UnmapBufferARB(GL_PIXEL_UNPACK_BUFFER_ARB); // release the mapped buffer
            assert(result == GL_TRUE);
            Q_UNUSED(result);
        }

        // bind the texture
        GL::BindTexture( target, texID );
        // copy pixels from PBO to texture object
        // Use offset instead of pointer (last parameter is 0).
        GL::TexSubImage2D(target,
                            0,              // level
                            roi.x1, roi.y1,               // xoffset, yoffset
                            roi.width(), roi.height(),
                            src.getGLTextureFormat(),            // format
                            src.getGLTextureType(),       // type
                            0);

        GL::BindBufferARB(GL_PIXEL_UNPACK_BUFFER_ARB, 0);
        GL::BindTexture(target, 0);
        glCheckError(GL);
    } else if ( (thisStorage != eStorageModeGLTex) && (otherStorage == eStorageModeGLTex) ) {
        // OpenGL texture to RAM image

        // only copy the intersection of roi, bounds and otherBounds
        RectI roi = srcRoi;
        bool doInteresect = roi.intersect(dstBounds, &roi);
        if (!doInteresect) {
            // no intersection between roi and the bounds of this image
            return;
        }
        doInteresect = roi.intersect(srcBounds, &roi);
        if (!doInteresect) {
            // no intersection between roi and the bounds of the other image
            return;
        }

        GLuint fboID = glContext->getOrCreateFBOId();

        int srcTarget = src.getGLTextureTarget();

        GL::BindFramebuffer(GL_FRAMEBUFFER, fboID);
        GL::Enable(srcTarget);
        GL::BindTexture( srcTarget, src.getGLTextureID() );
        GL::FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, srcTarget, src.getGLTextureID(), 0 /*LoD*/);
        //glViewport( 0, 0, srcBounds.width(), srcBounds.height() );
        GL::Viewport( roi.x1 - srcBounds.x1, roi.y1 - srcBounds.y1, roi.width(), roi.height() );
        glCheckFramebufferError(GL);
        // Ensure all drawing commands are finished
        GL::Flush();
        GL::Finish();
        glCheckError(GL);
        // Read to a temporary RGBA buffer then conver to the image which may not be RGBA
        ImagePtr tmpImg( new Image( ImageComponents::getRGBAComponents(), dst->getRoD(), roi, 0, dst->getPixelAspectRatio(), dst->getBitDepth(), dst->getPremultiplication(), dst->getFieldingOrder(), false, eStorageModeRAM, OSGLContextPtr(), GL_TEXTURE_2D, true) );

        {
            Image::WriteAccess tmpAcc(tmpImg ? tmpImg.get() : dst);
            unsigned char* data = tmpAcc.pixelAt(roi.x1, roi.y1);

            GL::ReadPixels(roi.x1 - srcBounds.x1, roi.y1 - srcBounds.y1, roi.width(), roi.height(), src.getGLTextureFormat(), src.getGLTextureType(), (GLvoid*)data);
            GL::BindTexture(srcTarget, 0);
        }
        GL::BindFramebuffer(GL_FRAMEBUFFER, 0);
        glCheckError(GL);

        // Ok now convert from RGBA to this image format if needed
        if ( tmpImg->getComponentsCount() != dst->getComponentsCount() ) {
            tmpImg->convertToFormat(roi, eViewerColorSpaceLinear, eViewerColorSpaceLinear, 3, false, false, dst);
        } else {
            dst->pasteFrom(*tmpImg, roi, false);
        }
    } 
    
}

void
Image::getABCDRectangles(const RectI& srcBounds, const RectI& biggerBounds, RectI& aRect, RectI& bRect, RectI& cRect, RectI& dRect)
{
    /*
     Compute the rectangles (A,B,C,D) where to set the image to 0

     AAAAAAAAAAAAAAAAAAAAAAAAAAAA
     AAAAAAAAAAAAAAAAAAAAAAAAAAAA
     DDDDDXXXXXXXXXXXXXXXXXXBBBBB
     DDDDDXXXXXXXXXXXXXXXXXXBBBBB
     DDDDDXXXXXXXXXXXXXXXXXXBBBBB
     DDDDDXXXXXXXXXXXXXXXXXXBBBBB
     CCCCCCCCCCCCCCCCCCCCCCCCCCCC
     CCCCCCCCCCCCCCCCCCCCCCCCCCCC
     */
    aRect.x1 = biggerBounds.x1;
    aRect.y1 = srcBounds.y2;
    aRect.y2 = biggerBounds.y2;
    aRect.x2 = biggerBounds.x2;

    bRect.x1 = srcBounds.x2;
    bRect.y1 = srcBounds.y1;
    bRect.x2 = biggerBounds.x2;
    bRect.y2 = srcBounds.y2;

    cRect.x1 = biggerBounds.x1;
    cRect.y1 = biggerBounds.y1;
    cRect.x2 = biggerBounds.x2;
    cRect.y2 = srcBounds.y1;

    dRect.x1 = biggerBounds.x1;
    dRect.y1 = srcBounds.y1;
    dRect.x2 = srcBounds.x1;
    dRect.y2 = srcBounds.y2;

}

void
Image::resizeInternal(const OSGLContextPtr& glContext,
                      const Image* srcImg,
                      const RectI& srcBounds,
                      const RectI& merge,
                      bool fillWithBlackAndTransparent,
                      bool setBitmapTo1,
                      bool createInCache,
                      ImagePtr* outputImage)
{
    ///Allocate to resized image
    if (!createInCache) {
        outputImage->reset( new Image( srcImg->getComponents(),
                                      srcImg->getRoD(),
                                      merge,
                                      srcImg->getMipMapLevel(),
                                      srcImg->getPixelAspectRatio(),
                                      srcImg->getBitDepth(),
                                      srcImg->getPremultiplication(),
                                      srcImg->getFieldingOrder(),
                                      srcImg->usesBitMap(),
                                      srcImg->getStorageMode(),
                                      glContext,
                                      srcImg->getGLTextureTarget(),
                                      srcImg->getParams()->getStorageInfo().isGPUTexture) );
    } else {
        ImageParamsPtr params( new ImageParams( *srcImg->getParams() ) );
        params->setBounds(merge);
        outputImage->reset( new Image( srcImg->getKey(), params, srcImg->getCacheAPI() ) );
        (*outputImage)->allocateMemory();
    }
    ImageBitDepthEnum depth = srcImg->getBitDepth();

    if (fillWithBlackAndTransparent) {

        if (srcImg->getStorageMode() == eStorageModeGLTex) {
            (*outputImage)->fillBoundsZero(glContext);
        } else {

            RectI aRect,bRect,cRect,dRect;
            getABCDRectangles(srcBounds, merge, aRect, bRect, cRect, dRect);
            Image::WriteAccess wacc( outputImage->get() );
            std::size_t pixelSize = srcImg->getComponentsCount() * getSizeOfForBitDepth(depth);

            if ( !aRect.isNull() ) {
                char* pix = (char*)wacc.pixelAt(aRect.x1, aRect.y1);
                assert(pix);
                double a = aRect.area();
                std::size_t memsize = a * pixelSize;
                std::memset(pix, 0, memsize);
                if ( setBitmapTo1 && (*outputImage)->usesBitMap() ) {
                    char* bm = wacc.bitmapAt(aRect.x1, aRect.y1);
                    assert(bm);
                    std::memset(bm, 1, a);
                }
            }
            if ( !cRect.isNull() ) {
                char* pix = (char*)wacc.pixelAt(cRect.x1, cRect.y1);
                assert(pix);
                double a = cRect.area();
                std::size_t memsize = a * pixelSize;
                std::memset(pix, 0, memsize);
                if ( setBitmapTo1 && (*outputImage)->usesBitMap() ) {
                    char* bm = (char*)wacc.bitmapAt(cRect.x1, cRect.y1);
                    assert(bm);
                    std::memset(bm, 1, a);
                }
            }
            if ( !bRect.isNull() ) {
                char* pix = (char*)wacc.pixelAt(bRect.x1, bRect.y1);
                assert(pix);
                int mw = merge.width();
                std::size_t rowsize = mw * pixelSize;
                int bw = bRect.width();
                std::size_t rectRowSize = bw * pixelSize;
                char* bm = ( setBitmapTo1 && (*outputImage)->usesBitMap() ) ? wacc.bitmapAt(bRect.x1, bRect.y1) : 0;
                for (int y = bRect.y1; y < bRect.y2; ++y, pix += rowsize) {
                    std::memset(pix, 0, rectRowSize);
                    if (bm) {
                        std::memset(bm, 1, bw);
                        bm += mw;
                    }
                }
            }
            if ( !dRect.isNull() ) {
                char* pix = (char*)wacc.pixelAt(dRect.x1, dRect.y1);
                assert(pix);
                int mw = merge.width();
                std::size_t rowsize = mw * pixelSize;
                int dw = dRect.width();
                std::size_t rectRowSize = dw * pixelSize;
                char* bm = ( setBitmapTo1 && (*outputImage)->usesBitMap() ) ? wacc.bitmapAt(dRect.x1, dRect.y1) : 0;
                for (int y = dRect.y1; y < dRect.y2; ++y, pix += rowsize) {
                    std::memset(pix, 0, rectRowSize);
                    if (bm) {
                        std::memset(bm, 1, dw);
                        bm += mw;
                    }
                }
            } // if (srcImg->getStorageMode() == eStorageModeGLTex) {
        }
    } // fillWithBlackAndTransparent

    if (srcImg->getStorageMode() == eStorageModeGLTex) {
        assert(glContext);
        if (glContext->isGPUContext()) {
            pasteFromGL<GL_GPU>(*srcImg, outputImage->get(), srcBounds, false, glContext, srcBounds, merge, (*outputImage)->getStorageMode(), srcImg->getStorageMode(), (*outputImage)->getGLTextureTarget());
        } else {
            pasteFromGL<GL_CPU>(*srcImg, outputImage->get(), srcBounds, false, glContext, srcBounds, merge, (*outputImage)->getStorageMode(), srcImg->getStorageMode(), (*outputImage)->getGLTextureTarget());
        }

        //pasteFrom(*srcImg, srcBounds, false, glContext);
    } else {
        switch (depth) {
            case eImageBitDepthByte:
                (*outputImage)->pasteFromForDepth<unsigned char>(*srcImg, srcBounds, srcImg->usesBitMap(), false);
                break;
            case eImageBitDepthShort:
                (*outputImage)->pasteFromForDepth<unsigned short>(*srcImg, srcBounds, srcImg->usesBitMap(), false);
                break;
            case eImageBitDepthHalf:
                assert(false);
                break;
            case eImageBitDepthFloat:
                (*outputImage)->pasteFromForDepth<float>(*srcImg, srcBounds, srcImg->usesBitMap(), false);
                break;
            case eImageBitDepthNone:
                break;
        }
    }
} // Image::resizeInternal

bool
Image::copyAndResizeIfNeeded(const RectI& newBounds,
                             bool fillWithBlackAndTransparent,
                             bool setBitmapTo1,
                             ImagePtr* output,
                             const OSGLContextPtr& glContext)
{
    assert(getStorageMode() != eStorageModeGLTex);
    if ( getBounds().contains(newBounds) ) {
        return false;
    }
    assert(output);

    QReadLocker k(&_entryLock);
    RectI merge = newBounds;
    merge.merge(_bounds);

    resizeInternal(glContext, this, _bounds, merge, fillWithBlackAndTransparent, setBitmapTo1, usesBitMap(), output);

    return true;
}

bool
Image::ensureBounds(const OSGLContextPtr& glContext,
                    const RectI& newBounds,
                    bool fillWithBlackAndTransparent,
                    bool setBitmapTo1)
{
    if ( getBounds().contains(newBounds) ) {
        return false;
    }

    QWriteLocker k(&_entryLock);
    RectI merge = newBounds;
    merge.merge(_bounds);

    ImagePtr tmpImg;
    resizeInternal(glContext, this, _bounds, merge, fillWithBlackAndTransparent, setBitmapTo1, false, &tmpImg);


    ///Change the size of the current buffer
    _bounds = merge;
    _params->setBounds(merge);
    assert( _bounds.contains(newBounds) );
    swapBuffer(*tmpImg);
    if ( usesBitMap() ) {
        _bitmap.swap(tmpImg->_bitmap);
    }

    return true;
}


// code proofread and fixed by @devernay on 8/8/2014
void
Image::pasteFrom(const Image & src,
                 const RectI & srcRoi,
                 bool copyBitmap,
                 const OSGLContextPtr& glContext)
{
    if (getStorageMode() == eStorageModeGLTex || src.getStorageMode() == eStorageModeGLTex) {
        assert(glContext);
        if (glContext->isGPUContext()) {
            pasteFromGL<GL_GPU>(src, this, srcRoi, copyBitmap, glContext, src.getBounds(), getBounds(), getStorageMode(), src.getStorageMode(), getGLTextureTarget());
        } else {
            pasteFromGL<GL_CPU>(src, this, srcRoi, copyBitmap, glContext, src.getBounds(), getBounds(), getStorageMode(), src.getStorageMode(), getGLTextureTarget());
        }
    } else {
        assert(getStorageMode() != eStorageModeGLTex && src.getStorageMode() != eStorageModeGLTex);
        ImageBitDepthEnum depth = getBitDepth();

        switch (depth) {
        case eImageBitDepthByte:
            pasteFromForDepth<unsigned char>(src, srcRoi, copyBitmap, true);
            break;
        case eImageBitDepthShort:
            pasteFromForDepth<unsigned short>(src, srcRoi, copyBitmap, true);
            break;
        case eImageBitDepthHalf:
            assert(false);
            break;
        case eImageBitDepthFloat:
            pasteFromForDepth<float>(src, srcRoi, copyBitmap, true);
            break;
        case eImageBitDepthNone:
            break;
        }
    }
} // pasteFrom

template <typename PIX, int maxValue, int nComps>
void
Image::fillForDepthForComponents(const RectI & roi_,
                                 float r,
                                 float g,
                                 float b,
                                 float a)
{
    assert( (getBitDepth() == eImageBitDepthByte && sizeof(PIX) == 1) || (getBitDepth() == eImageBitDepthShort && sizeof(PIX) == 2) || (getBitDepth() == eImageBitDepthFloat && sizeof(PIX) == 4) );

    RectI roi = roi_;
    bool doInteresect = roi.intersect(_bounds, &roi);
    if (!doInteresect) {
        // no intersection between roi and the bounds of the image
        return;
    }

    int rowElems = (int)getComponentsCount() * _bounds.width();
    const float fillValue[4] = {
        nComps == 1 ? a * maxValue : r * maxValue, g * maxValue, b * maxValue, a * maxValue
    };


    // now we're safe: the image contains the area in roi
    PIX* dst = (PIX*)pixelAt(roi.x1, roi.y1);
    for ( int i = 0; i < roi.height(); ++i, dst += (rowElems - roi.width() * nComps) ) {
        for (int j = 0; j < roi.width(); ++j, dst += nComps) {
            for (int k = 0; k < nComps; ++k) {
                dst[k] = fillValue[k];
            }
        }
    }
}

// code proofread and fixed by @devernay on 8/8/2014
template <typename PIX, int maxValue>
void
Image::fillForDepth(const RectI & roi_,
                    float r,
                    float g,
                    float b,
                    float a)
{
    switch (_nbComponents) {
    case 0:

        return;
        break;
    case 1:
        fillForDepthForComponents<PIX, maxValue, 1>(roi_, r, g, b, a);
        break;
    case 2:
        fillForDepthForComponents<PIX, maxValue, 2>(roi_, r, g, b, a);
        break;
    case 3:
        fillForDepthForComponents<PIX, maxValue, 3>(roi_, r, g, b, a);
        break;
    case 4:
        fillForDepthForComponents<PIX, maxValue, 4>(roi_, r, g, b, a);
        break;
    default:
        break;
    }
}

template <typename GL>
void fillGL(const RectI & roi,
            float r,
            float g,
            float b,
            float a,
            const OSGLContextPtr& glContext,
            const RectI& bounds,
            int target,
            int texID)
{
    RectI realRoI = roi;
    bool doInteresect = roi.intersect(bounds, &realRoI);
    if (!doInteresect) {
        // no intersection between roi and the bounds of the image
        return;
    }

    assert(glContext);

    GLuint fboID = glContext->getOrCreateFBOId();

    GL::BindFramebuffer(GL_FRAMEBUFFER, fboID);
    GL::Enable(target);
    GL::ActiveTexture(GL_TEXTURE0);
    GL::BindTexture( target, texID );

    GL::TexParameteri (target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    GL::TexParameteri (target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    GL::TexParameteri (target, GL_TEXTURE_WRAP_S, GL_REPEAT);
    GL::TexParameteri (target, GL_TEXTURE_WRAP_T, GL_REPEAT);

    GL::FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, target, texID, 0 /*LoD*/);
    glCheckFramebufferError(GL);

    Image::setupGLViewport<GL>(bounds, roi);
    GL::ClearColor(r, g, b, a);
    GL::Clear(GL_COLOR_BUFFER_BIT);

    GL::BindTexture(target, 0);
    glCheckError(GL);
}

// code proofread and fixed by @devernay on 8/8/2014
void
Image::fill(const RectI & roi,
            float r,
            float g,
            float b,
            float a,
            const OSGLContextPtr& glContext)
{
    QWriteLocker k(&_entryLock);

    if (getStorageMode() == eStorageModeGLTex) {
        if (glContext->isGPUContext()) {
            fillGL<GL_GPU>(roi, r, g, b, a, glContext, _bounds, getGLTextureTarget(), getGLTextureID());
        } else {
            fillGL<GL_CPU>(roi, r, g, b, a, glContext, _bounds, getGLTextureTarget(), getGLTextureID());
        }
        return;
    }

    switch ( getBitDepth() ) {
    case eImageBitDepthByte:
        fillForDepth<unsigned char, 255>(roi, r, g, b, a);
        break;
    case eImageBitDepthShort:
        fillForDepth<unsigned short, 65535>(roi, r, g, b, a);
        break;
    case eImageBitDepthHalf:
        assert(false);
        break;
    case eImageBitDepthFloat:
        fillForDepth<float, 1>(roi, r, g, b, a);
        break;
    case eImageBitDepthNone:
        break;
    }
} // fill

void
Image::fillZero(const RectI& roi,
                const OSGLContextPtr& glContext)
{
    if (getStorageMode() == eStorageModeGLTex) {
        fill(roi, 0., 0., 0., 0., glContext);

        return;
    }

    QWriteLocker k(&_entryLock);
    RectI intersection;

    if ( !roi.intersect(_bounds, &intersection) ) {
        return;
    }


    std::size_t rowSize =  (std::size_t)_nbComponents;
    switch ( getBitDepth() ) {
    case eImageBitDepthByte:
        rowSize *= sizeof(unsigned char);
        break;
    case eImageBitDepthShort:
        rowSize *= sizeof(unsigned short);
        break;
    case eImageBitDepthHalf:
        rowSize *= sizeof(unsigned short);
        break;
    case eImageBitDepthFloat:
        rowSize *= sizeof(float);
        break;
    case eImageBitDepthNone:

        return;
    }

    std::size_t roiMemSize = rowSize * intersection.width();
    rowSize *= _bounds.width();

    char* dstPixels = (char*)pixelAt(intersection.x1, intersection.y1);
    assert(dstPixels);
    for (int y = intersection.y1; y < intersection.y2; ++y, dstPixels += rowSize) {
        std::memset(dstPixels, 0, roiMemSize);
    }
}

void
Image::fillBoundsZero(const OSGLContextPtr& glContext)
{
    if (getStorageMode() == eStorageModeGLTex) {
        fill(getBounds(), 0., 0., 0., 0., glContext);

        return;
    }

    QWriteLocker k(&_entryLock);
    std::size_t rowSize =  (std::size_t)_nbComponents;

    switch ( getBitDepth() ) {
    case eImageBitDepthByte:
        rowSize *= sizeof(unsigned char);
        break;
    case eImageBitDepthShort:
        rowSize *= sizeof(unsigned short);
        break;
    case eImageBitDepthHalf:
        rowSize *= sizeof(unsigned short);
        break;
    case eImageBitDepthFloat:
        rowSize *= sizeof(float);
        break;
    case eImageBitDepthNone:

        return;
    }

    std::size_t roiMemSize = rowSize * _bounds.width() * _bounds.height();
    char* dstPixels = (char*)pixelAt(_bounds.x1, _bounds.y1);
    std::memset(dstPixels, 0, roiMemSize);
}

unsigned char*
Image::pixelAt(int x,
               int y)
{
    if ( ( x < _bounds.x1 ) || ( x >= _bounds.x2 ) || ( y < _bounds.y1 ) || ( y >= _bounds.y2 ) ) {
        return NULL;
    } else {
        unsigned char* ret =  (unsigned char*)this->_data.writable();
        if (!ret) {
            return 0;
        }
        int compDataSize = _depthBytesSize * _nbComponents;
        ret = ret + (qint64)( y - _bounds.y1 ) * compDataSize * _bounds.width()
              + (qint64)( x - _bounds.x1 ) * compDataSize;

        return ret;
    }
}

unsigned char*
Image::pixelAtStatic(int x,
                     int y,
                     const RectI& bounds,
                     int nComps,
                     int dataSizeOf,
                     unsigned char* buf)
{
    if ( ( x < bounds.x1 ) || ( x >= bounds.x2 ) || ( y < bounds.y1 ) || ( y >= bounds.y2 ) ) {
        return NULL;
    } else {
        unsigned char* ret = buf;
        if (!ret) {
            return 0;
        }
        int compDataSize = dataSizeOf * nComps;
        ret = ret + (qint64)( y - bounds.y1 ) * compDataSize * bounds.width()
              + (qint64)( x - bounds.x1 ) * compDataSize;

        return ret;
    }
}

const unsigned char*
Image::pixelAt(int x,
               int y) const
{
    if ( ( x < _bounds.x1 ) || ( x >= _bounds.x2 ) || ( y < _bounds.y1 ) || ( y >= _bounds.y2 ) ) {
        return NULL;
    } else {
        unsigned char* ret = (unsigned char*)this->_data.readable();
        if (!ret) {
            return 0;
        }
        int compDataSize = _depthBytesSize * _nbComponents;
        ret = ret + (qint64)( y - _bounds.y1 ) * compDataSize * _bounds.width()
              + (qint64)( x - _bounds.x1 ) * compDataSize;

        return ret;
    }
}

unsigned int
Image::getComponentsCount() const
{
    return _nbComponents;
}

bool
Image::hasEnoughDataToConvert(ImageComponentsEnum from,
                              ImageComponentsEnum to)
{
    switch (from) {
    case eImageComponentRGBA:

        return true;
    case eImageComponentRGB: {
        switch (to) {
        case eImageComponentRGBA:

            return true; //< let RGB fill the alpha with 0
        case eImageComponentRGB:

            return true;
        case eImageComponentAlpha:

            return false;
        default:

            return false;
        }
        break;
    }
    case eImageComponentAlpha: {
        switch (to) {
        case eImageComponentRGBA:

            return false;
        case eImageComponentRGB:

            return false;
        case eImageComponentAlpha:

            return true;
        default:

            return false;
        }
        break;
    }
    default:

        return false;
        break;
    }
}

std::string
Image::getFormatString(const ImageComponents& comps,
                       ImageBitDepthEnum depth)
{
    std::string s = comps.getLayerName() + '.' + comps.getComponentsGlobalName();

    s.append( getDepthString(depth) );

    return s;
}

std::string
Image::getDepthString(ImageBitDepthEnum depth)
{
    std::string s;

    switch (depth) {
    case eImageBitDepthByte:
        s += "8u";
        break;
    case eImageBitDepthShort:
        s += "16u";
        break;
    case eImageBitDepthHalf:
        s += "16f";
        break;
    case eImageBitDepthFloat:
        s += "32f";
        break;
    case eImageBitDepthNone:
        break;
    }

    return s;
}

bool
Image::isBitDepthConversionLossy(ImageBitDepthEnum from,
                                 ImageBitDepthEnum to)
{
    int sizeOfFrom = getSizeOfForBitDepth(from);
    int sizeOfTo = getSizeOfForBitDepth(to);

    return sizeOfTo < sizeOfFrom;
}

double
Image::getPixelAspectRatio() const
{
    return this->_par;
}

ImageFieldingOrderEnum
Image::getFieldingOrder() const
{
    return this->_fielding;
}

ImagePremultiplicationEnum
Image::getPremultiplication() const
{
    return this->_premult;
}

unsigned int
Image::getRowElements() const
{
    QReadLocker k(&_entryLock);

    return getComponentsCount() * _bounds.width();
}

// code proofread and fixed by @devernay on 4/12/2014
template <typename PIX, int maxValue>
void
Image::halveRoIForDepth(const RectI & roi,
                        bool copyBitMap,
                        Image* output) const
{
    assert( (getBitDepth() == eImageBitDepthByte && sizeof(PIX) == 1) ||
            (getBitDepth() == eImageBitDepthShort && sizeof(PIX) == 2) ||
            (getBitDepth() == eImageBitDepthFloat && sizeof(PIX) == 4) );

    ///handle case where there is only 1 column/row
    if ( (roi.width() == 1) || (roi.height() == 1) ) {
        assert( !(roi.width() == 1 && roi.height() == 1) ); /// can't be 1x1
        halve1DImage(roi, output);

        return;
    }

    /// Take the lock for both bitmaps since we're about to read/write from them!
    QWriteLocker k1(&output->_entryLock);
    QReadLocker k2(&_entryLock);

    ///The source rectangle, intersected to this image region of definition in pixels
    const RectI &srcBounds = _bounds;
    const RectI &dstBounds = output->_bounds;
    const RectI &srcBmBounds = _bitmap.getBounds();
    const RectI &dstBmBounds = output->_bitmap.getBounds();
    assert( !copyBitMap || usesBitMap() );
    assert( !usesBitMap() || (srcBmBounds == srcBounds && dstBmBounds == dstBounds) );

    // the srcRoD of the output should be enclosed in half the roi.
    // It does not have to be exactly half of the input.
    //    assert(dstRoD.x1*2 >= roi.x1 &&
    //           dstRoD.x2*2 <= roi.x2 &&
    //           dstRoD.y1*2 >= roi.y1 &&
    //           dstRoD.y2*2 <= roi.y2 &&
    //           dstRoD.width()*2 <= roi.width() &&
    //           dstRoD.height()*2 <= roi.height());
    assert( getComponents() == output->getComponents() );

    RectI dstRoI;
    RectI srcRoI = roi;
    srcRoI.intersect(srcBounds, &srcRoI); // intersect srcRoI with the region of definition

    dstRoI.x1 = std::floor(srcRoI.x1 / 2.);
    dstRoI.y1 = std::floor(srcRoI.y1 / 2.);
    dstRoI.x2 = std::ceil(srcRoI.x2 / 2.);
    dstRoI.y2 = std::ceil(srcRoI.y2 / 2.);


    const PIX* const srcPixels      = (const PIX*)pixelAt(srcBounds.x1,   srcBounds.y1);
    const char* const srcBmPixels   = _bitmap.getBitmapAt(srcBmBounds.x1, srcBmBounds.y1);
    PIX* const dstPixels          = (PIX*)output->pixelAt(dstBounds.x1,   dstBounds.y1);
    char* const dstBmPixels = output->_bitmap.getBitmapAt(dstBmBounds.x1, dstBmBounds.y1);
    int srcRowSize = srcBounds.width() * _nbComponents;
    int dstRowSize = dstBounds.width() * _nbComponents;

    // offset pointers so that srcData and dstData correspond to pixel (0,0)
    const PIX* const srcData = srcPixels - (srcBounds.x1 * _nbComponents + srcRowSize * srcBounds.y1);
    PIX* const dstData       = dstPixels - (dstBounds.x1 * _nbComponents + dstRowSize * dstBounds.y1);
    const int srcBmRowSize = srcBmBounds.width();
    const int dstBmRowSize = dstBmBounds.width();
    const char* const srcBmData = srcBmPixels - (srcBmBounds.x1 + srcBmRowSize * srcBmBounds.y1);
    char* const dstBmData       = dstBmPixels - (dstBmBounds.x1 + dstBmRowSize * dstBmBounds.y1);

    for (int y = dstRoI.y1; y < dstRoI.y2; ++y) {
        const PIX* const srcLineStart    = srcData + y * 2 * srcRowSize;
        PIX* const dstLineStart          = dstData + y     * dstRowSize;
        const char* const srcBmLineStart = srcBmData + y * 2 * srcBmRowSize;
        char* const dstBmLineStart       = dstBmData + y     * dstBmRowSize;

        // The current dst row, at y, covers the src rows y*2 (thisRow) and y*2+1 (nextRow).
        // Check that if are within srcBounds.
        int srcy = y * 2;
        bool pickThisRow = srcBounds.y1 <= (srcy + 0) && (srcy + 0) < srcBounds.y2;
        bool pickNextRow = srcBounds.y1 <= (srcy + 1) && (srcy + 1) < srcBounds.y2;
        int sumH = (int)pickNextRow + (int)pickThisRow;
        assert(sumH == 1 || sumH == 2);

        for (int x = dstRoI.x1; x < dstRoI.x2; ++x) {
            const PIX* const srcPixStart    = srcLineStart   + x * 2 * _nbComponents;
            const char* const srcBmPixStart = srcBmLineStart + x * 2;
            PIX* const dstPixStart          = dstLineStart   + x * _nbComponents;
            char* const dstBmPixStart       = dstBmLineStart + x;

            // The current dst col, at y, covers the src cols x*2 (thisCol) and x*2+1 (nextCol).
            // Check that if are within srcBounds.
            int srcx = x * 2;
            bool pickThisCol = srcBounds.x1 <= (srcx + 0) && (srcx + 0) < srcBounds.x2;
            bool pickNextCol = srcBounds.x1 <= (srcx + 1) && (srcx + 1) < srcBounds.x2;
            int sumW = (int)pickThisCol + (int)pickNextCol;
            assert(sumW == 1 || sumW == 2);
            const int sum = sumW * sumH;
            assert(0 < sum && sum <= 4);

            if (sum == 0) { // never happens
                for (int k = 0; k < _nbComponents; ++k) {
                    dstPixStart[k] = 0;
                }
                if (copyBitMap) {
                    dstBmPixStart[0] = 0;
                }
                continue;
            }

            for (int k = 0; k < _nbComponents; ++k) {
                ///a b
                ///c d

                const PIX a = (pickThisCol && pickThisRow) ? *(srcPixStart + k) : 0;
                const PIX b = (pickNextCol && pickThisRow) ? *(srcPixStart + k + _nbComponents) : 0;
                const PIX c = (pickThisCol && pickNextRow) ? *(srcPixStart + k + srcRowSize) : 0;
                const PIX d = (pickNextCol && pickNextRow) ? *(srcPixStart + k + srcRowSize  + _nbComponents)  : 0;

                assert( sumW == 2 || ( sumW == 1 && ( (a == 0 && c == 0) || (b == 0 && d == 0) ) ) );
                assert( sumH == 2 || ( sumH == 1 && ( (a == 0 && b == 0) || (c == 0 && d == 0) ) ) );
                dstPixStart[k] = (a + b + c + d) / sum;
            }

            if (copyBitMap) {
                ///a b
                ///c d

                char a = (pickThisCol && pickThisRow) ? *(srcBmPixStart) : 0;
                char b = (pickNextCol && pickThisRow) ? *(srcBmPixStart + 1) : 0;
                char c = (pickThisCol && pickNextRow) ? *(srcBmPixStart + srcBmRowSize) : 0;
                char d = (pickNextCol && pickNextRow) ? *(srcBmPixStart + srcBmRowSize  + 1)  : 0;
#if NATRON_ENABLE_TRIMAP
                /*
                   The only correct solution is to convert pixels being rendered to 0 otherwise the caller
                   would have to wait for the original fullscale image render to be finished and then re-downscale again.
                 */
                if (a == PIXEL_UNAVAILABLE) {
                    a = 0;
                }
                if (b == PIXEL_UNAVAILABLE) {
                    b = 0;
                }
                if (c == PIXEL_UNAVAILABLE) {
                    c = 0;
                }
                if (d == PIXEL_UNAVAILABLE) {
                    d = 0;
                }
#endif
                assert( sumW == 2 || ( sumW == 1 && ( (a == 0 && c == 0) || (b == 0 && d == 0) ) ) );
                assert( sumH == 2 || ( sumH == 1 && ( (a == 0 && b == 0) || (c == 0 && d == 0) ) ) );
                assert(a + b + c + d <= sum); // bitmaps are 0 or 1
                // the following is an integer division, the result can be 0 or 1
                dstBmPixStart[0] = (a + b + c + d) / sum;
                assert(dstBmPixStart[0] == 0 || dstBmPixStart[0] == 1);
            }
        }
    }
} // halveRoIForDepth

// code proofread and fixed by @devernay on 8/8/2014
void
Image::halveRoI(const RectI & roi,
                bool copyBitMap,
                Image* output) const
{
    switch ( getBitDepth() ) {
    case eImageBitDepthByte:
        halveRoIForDepth<unsigned char, 255>(roi, copyBitMap,  output);
        break;
    case eImageBitDepthShort:
        halveRoIForDepth<unsigned short, 65535>(roi, copyBitMap, output);
        break;
    case eImageBitDepthHalf:
        assert(false);
        break;
    case eImageBitDepthFloat:
        halveRoIForDepth<float, 1>(roi, copyBitMap, output);
        break;
    case eImageBitDepthNone:
        break;
    }
}

// code proofread and fixed by @devernay on 8/8/2014
template <typename PIX, int maxValue>
void
Image::halve1DImageForDepth(const RectI & roi,
                            Image* output) const
{
    int width = roi.width();
    int height = roi.height();

    assert(width == 1 || height == 1); /// must be 1D
    assert( output->getComponents() == getComponents() );

    /// Take the lock for both bitmaps since we're about to read/write from them!
    QWriteLocker k1(&output->_entryLock);
    QReadLocker k2(&_entryLock);
    const RectI & srcBounds = _bounds;
    const RectI & dstBounds = output->_bounds;
//    assert(dstBounds.x1 * 2 == roi.x1 &&
//           dstBounds.y1 * 2 == roi.y1 &&
//           (
//               dstBounds.x2 * 2 == roi.x2 || // we halve in only 1 dimension
//               dstBounds.y2 * 2 == roi.y2)
//           );
    int halfWidth = width / 2;
    int halfHeight = height / 2;
    if (height == 1) { //1 row
        assert(width != 1); /// widthxheight can't be 1x1

        const PIX* src = (const PIX*)pixelAt(roi.x1, roi.y1);
        PIX* dst = (PIX*)output->pixelAt(dstBounds.x1, dstBounds.y1);
        assert(src && dst);
        for (int x = 0; x < halfWidth; ++x) {
            for (int k = 0; k < _nbComponents; ++k) {
                *dst++ = PIX( (float)( *src + *(src + _nbComponents) ) / 2. );
                ++src;
            }
            src += _nbComponents;
        }
    } else if (width == 1) {
        int rowSize = srcBounds.width() * _nbComponents;
        const PIX* src = (const PIX*)pixelAt(roi.x1, roi.y1);
        PIX* dst = (PIX*)output->pixelAt(dstBounds.x1, dstBounds.y1);
        assert(src && dst);
        for (int y = 0; y < halfHeight; ++y) {
            for (int k = 0; k < _nbComponents; ++k) {
                *dst++ = PIX( (float)( *src + (*src + rowSize) ) / 2. );
                ++src;
            }
            src += rowSize;
        }
    }
}

// code proofread and fixed by @devernay on 8/8/2014
void
Image::halve1DImage(const RectI & roi,
                    Image* output) const
{
    switch ( getBitDepth() ) {
    case eImageBitDepthByte:
        halve1DImageForDepth<unsigned char, 255>(roi, output);
        break;
    case eImageBitDepthShort:
        halve1DImageForDepth<unsigned short, 65535>(roi, output);
        break;
    case eImageBitDepthHalf:
        assert(false);
        break;
    case eImageBitDepthFloat:
        halve1DImageForDepth<float, 1>(roi, output);
        break;
    case eImageBitDepthNone:
        break;
    }
}

// code proofread and fixed by @devernay on 8/8/2014
void
Image::downscaleMipMap(const RectD& dstRod,
                       const RectI & roi,
                       unsigned int fromLevel,
                       unsigned int toLevel,
                       bool copyBitMap,
                       Image* output) const
{
    assert(getStorageMode() != eStorageModeGLTex);

    ///You should not call this function with a level equal to 0.
    assert(toLevel >  fromLevel);

    assert(_bounds.x1 <= roi.x1 && roi.x2 <= _bounds.x2 &&
           _bounds.y1 <= roi.y1 && roi.y2 <= _bounds.y2);
    double par = getPixelAspectRatio();
//    RectD roiCanonical;
//    roi.toCanonical(fromLevel, par , dstRod, &roiCanonical);
//    RectI dstRoI;
//    roiCanonical.toPixelEnclosing(toLevel, par , &dstRoI);
    unsigned int downscaleLvls = toLevel - fromLevel;

    assert( !copyBitMap || _bitmap.getBitmap() );

    RectI dstRoI  = roi.downscalePowerOfTwoSmallestEnclosing(downscaleLvls);
    ImagePtr tmpImg( new Image( getComponents(), dstRod, dstRoI, toLevel, par, getBitDepth(), getPremultiplication(), getFieldingOrder(), true, eStorageModeRAM, OSGLContextPtr(), GL_TEXTURE_2D, true) );

    buildMipMapLevel( dstRod, roi, downscaleLvls, copyBitMap, tmpImg.get() );

    // check that the downscaled mipmap is inside the output image (it may not be equal to it)
    assert(dstRoI.x1 >= output->_bounds.x1);
    assert(dstRoI.x2 <= output->_bounds.x2);
    assert(dstRoI.y1 >= output->_bounds.y1);
    assert(dstRoI.y2 <= output->_bounds.y2);

    ///Now copy the result of tmpImg into the output image
    output->pasteFrom(*tmpImg, dstRoI, copyBitMap);
}

bool
Image::checkForNaNs(const RectI& roi)
{
    if (getBitDepth() != eImageBitDepthFloat) {
        return false;
    }
    if (getStorageMode() == eStorageModeGLTex) {
        return false;
    }

    QWriteLocker k(&_entryLock);
    unsigned int compsCount = getComponentsCount();
    bool hasnan = false;
    for (int y = roi.y1; y < roi.y2; ++y) {
        float* pix = (float*)pixelAt(roi.x1, y);
        float* const end = pix +  compsCount * roi.width();

        for (; pix < end; ++pix) {
            // we remove NaNs, but infinity values should pose no problem
            // (if they do, please explain here which ones)
            if (*pix != *pix) { // check for NaN
                *pix = 1.;
                hasnan = true;
            }
        }
    }

    return hasnan;
}

// code proofread and fixed by @devernay on 8/8/2014
template <typename PIX, int maxValue>
void
Image::upscaleMipMapForDepth(const RectI & roi,
                             unsigned int fromLevel,
                             unsigned int toLevel,
                             Image* output) const
{
    assert( getBitDepth() == output->getBitDepth() );
    assert( (getBitDepth() == eImageBitDepthByte && sizeof(PIX) == 1) || (getBitDepth() == eImageBitDepthShort && sizeof(PIX) == 2) || (getBitDepth() == eImageBitDepthFloat && sizeof(PIX) == 4) );

    ///You should not call this function with a level equal to 0.
    assert(fromLevel > toLevel);

    assert(roi.x1 <= _bounds.x1 && _bounds.x2 <= roi.x2 &&
           roi.y1 <= _bounds.y1 && _bounds.y2 <= roi.y2);

    ///The source rectangle, intersected to this image region of definition in pixels
    RectD roiCanonical;
    roi.toCanonical(fromLevel, _par, getRoD(), &roiCanonical);
    RectI dstRoi;
    roiCanonical.toPixelEnclosing(toLevel, _par, &dstRoi);

    const RectI & srcRoi = roi;

    dstRoi.intersect(output->_bounds, &dstRoi); //output may be a bit smaller than the upscaled RoI
    int scale = 1 << (fromLevel - toLevel);

    assert( output->getComponents() == getComponents() );

    if (_nbComponents == 0) {
        return;
    }

    QWriteLocker k1(&output->_entryLock);
    QReadLocker k2(&_entryLock);
    int srcRowSize = _bounds.width() * _nbComponents;
    int dstRowSize = output->_bounds.width() * _nbComponents;
    const PIX *src = (const PIX*)pixelAt(srcRoi.x1, srcRoi.y1);
    PIX* dst = (PIX*)output->pixelAt(dstRoi.x1, dstRoi.y1);
    assert(src && dst);

    // algorithm: fill the first line of output, and replicate it as many times as necessary
    // works even if dstRoi is not exactly a multiple of srcRoi (first/last column/line may not be complete)
    int yi = srcRoi.y1;
    int ycount; // how many lines should be filled
    for (int yo = dstRoi.y1; yo < dstRoi.y2; ++yi, src += srcRowSize, yo += ycount, dst += ycount * dstRowSize) {
        const PIX * const srcLineStart = src;
        PIX * const dstLineBatchStart = dst;
        ycount = scale - ((yo - dstRoi.y1) - (yi - srcRoi.y1) * scale); // how many lines should be filled
        ycount = std::min(ycount, dstRoi.y2 - yo);
        assert(0 < ycount && ycount <= scale);
        int xi = srcRoi.x1;
        int xcount = 0; // how many pixels should be filled
        const PIX * srcPix = srcLineStart;
        PIX * dstPixFirst = dstLineBatchStart;
        // fill the first line
        for (int xo = dstRoi.x1; xo < dstRoi.x2; ++xi, srcPix += _nbComponents, xo += xcount, dstPixFirst += xcount * _nbComponents) {
            xcount = scale - ((xo - dstRoi.x1) - (xi - srcRoi.x1) * scale);
            xcount = std::min(xcount, dstRoi.x2 - xo);
            //assert(0 < xcount && xcount <= scale);
            // replicate srcPix as many times as necessary
            PIX * dstPix = dstPixFirst;
            //assert((srcPix-(PIX*)pixelAt(srcRoi.x1, srcRoi.y1)) % components == 0);
            for (int i = 0; i < xcount; ++i, dstPix += _nbComponents) {
                assert( ( dstPix - (PIX*)output->pixelAt(dstRoi.x1, dstRoi.y1) ) % _nbComponents == 0 );
                assert(dstPix >= (PIX*)output->pixelAt(xo, yo) && dstPix < (PIX*)output->pixelAt(xo, yo) + xcount * _nbComponents);
                for (int c = 0; c < _nbComponents; ++c) {
                    dstPix[c] = srcPix[c];
                }
            }
            //assert(dstPix == dstPixFirst + xcount*components);
        }
        PIX * dstLineStart = dstLineBatchStart + dstRowSize; // first line was filled already
        // now replicate the line as many times as necessary
        for (int i = 1; i < ycount; ++i, dstLineStart += dstRowSize) {
            std::copy(dstLineBatchStart, dstLineBatchStart + dstRowSize, dstLineStart);
        }
    }
} // upscaleMipMapForDepth

// code proofread and fixed by @devernay on 8/8/2014
void
Image::upscaleMipMap(const RectI & roi,
                     unsigned int fromLevel,
                     unsigned int toLevel,
                     Image* output) const
{
    assert(getStorageMode() != eStorageModeGLTex);

    switch ( getBitDepth() ) {
    case eImageBitDepthByte:
        upscaleMipMapForDepth<unsigned char, 255>(roi, fromLevel, toLevel, output);
        break;
    case eImageBitDepthShort:
        upscaleMipMapForDepth<unsigned short, 65535>(roi, fromLevel, toLevel, output);
        break;
    case eImageBitDepthHalf:
        assert(false);
        break;
    case eImageBitDepthFloat:
        upscaleMipMapForDepth<float, 1>(roi, fromLevel, toLevel, output);
        break;
    case eImageBitDepthNone:
        break;
    }
}

// code proofread and fixed by @devernay on 8/8/2014
void
Image::buildMipMapLevel(const RectD& dstRoD,
                        const RectI & roi,
                        unsigned int level,
                        bool copyBitMap,
                        Image* output) const
{
    ///The last mip map level we will make with closestPo2
    RectI lastLevelRoI = roi.downscalePowerOfTwoSmallestEnclosing(level);

    ///The output image must contain the last level roi
    assert( output->getBounds().contains(lastLevelRoI) );

    assert( output->getComponents() == getComponents() );

    if (level == 0) {
        ///Just copy the roi and return
        output->pasteFrom(*this, roi, copyBitMap);

        return;
    }

    const Image* srcImg = this;
    Image* dstImg = NULL;
    bool mustFreeSrc = false;
    RectI previousRoI = roi;
    ///Build all the mipmap levels until we reach the one we are interested in
    for (unsigned int i = 1; i <= level; ++i) {
        ///Halve the smallest enclosing po2 rect as we need to render a minimum of the renderWindow
        RectI halvedRoI = previousRoI.downscalePowerOfTwoSmallestEnclosing(1);

        ///Allocate an image with half the size of the source image
        dstImg = new Image( getComponents(), dstRoD, halvedRoI, getMipMapLevel() + i, getPixelAspectRatio(), getBitDepth(), getPremultiplication(), getFieldingOrder(), true, eStorageModeRAM, OSGLContextPtr(), GL_TEXTURE_2D, true);

        ///Half the source image into dstImg.
        ///We pass the closestPo2 roi which might not be the entire size of the source image
        ///If the source image'sroi was originally a po2.
        srcImg->halveRoI(previousRoI, copyBitMap, dstImg);

        ///Clean-up, we should use shared_ptrs for safety
        if (mustFreeSrc) {
            delete srcImg;
        }

        ///Switch for next pass
        previousRoI = halvedRoI;
        srcImg = dstImg;
        mustFreeSrc = true;
    }

    assert(srcImg->getBounds() == lastLevelRoI);

    ///Finally copy the last mipmap level into output.
    output->pasteFrom( *srcImg, srcImg->getBounds(), copyBitMap);

    ///Clean-up, we should use shared_ptrs for safety
    if (mustFreeSrc) {
        delete srcImg;
    }
} // buildMipMapLevel

double
Image::getScaleFromMipMapLevel(unsigned int level)
{
    return 1. / (1 << level);
}

#ifndef M_LN2
#define M_LN2       0.693147180559945309417232121458176568  /* loge(2)        */
#endif
unsigned int
Image::getLevelFromScale(double s)
{
    assert(0. < s && s <= 1.);
    int retval = -std::floor(std::log(s) / M_LN2 + 0.5);
    assert(retval >= 0);

    return retval;
}

void
Image::copyBitmapRowPortion(int x1,
                            int x2,
                            int y,
                            const Image& other)
{
    _bitmap.copyRowPortion(x1, x2, y, other._bitmap);
}

void
Bitmap::copyRowPortion(int x1,
                       int x2,
                       int y,
                       const Bitmap& other)
{
    const char* srcBitmap = other.getBitmapAt(x1, y);
    char* dstBitmap = getBitmapAt(x1, y);
    const char* end = dstBitmap + (x2 - x1);

    while (dstBitmap < end) {
        *dstBitmap = /**srcBitmap == PIXEL_UNAVAILABLE ? 0 : */ *srcBitmap;
        ++dstBitmap;
        ++srcBitmap;
    }
}

void
Image::copyBitmapPortion(const RectI& roi,
                         const Image& other)
{
    _bitmap.copyBitmapPortion(roi, other._bitmap);
}

void
Bitmap::copyBitmapPortion(const RectI& roi,
                          const Bitmap& other)
{
    assert(roi.x1 >= _bounds.x1 && roi.x2 <= _bounds.x2 && roi.y1 >= _bounds.y1 && roi.y2 <= _bounds.y2);
    assert(roi.x1 >= other._bounds.x1 && roi.x2 <= other._bounds.x2 && roi.y1 >= other._bounds.y1 && roi.y2 <= other._bounds.y2);

    int srcRowSize = other._bounds.width();
    int dstRowSize = _bounds.width();
    const char* srcBitmap = other.getBitmapAt(roi.x1, roi.y1);
    char* dstBitmap = getBitmapAt(roi.x1, roi.y1);

    for (int y = roi.y1; y < roi.y2; ++y,
         srcBitmap += srcRowSize,
         dstBitmap += dstRowSize) {
        const char* srcCur = srcBitmap;
        const char* srcEnd = srcBitmap + roi.width();
        char* dstCur = dstBitmap;
        while (srcCur < srcEnd) {
            *dstCur = /**srcCur == PIXEL_UNAVAILABLE ? 0 : */ *srcCur;
            ++srcCur;
            ++dstCur;
        }
    }
}

template <typename PIX, bool doPremult>
void
Image::premultInternal(const RectI& roi)
{
    WriteAccess acc(this);
    RectI renderWindow;

    roi.intersect(_bounds, &renderWindow);

    assert(getComponentsCount() == 4);

    int srcRowElements = 4 * _bounds.width();
    PIX* dstPix = (PIX*)acc.pixelAt(renderWindow.x1, renderWindow.y1);
    for ( int y = renderWindow.y1; y < renderWindow.y2; ++y, dstPix += (srcRowElements - (renderWindow.x2 - renderWindow.x1) * 4) ) {
        for (int x = renderWindow.x1; x < renderWindow.x2; ++x, dstPix += 4) {
            for (int c = 0; c < 3; ++c) {
                if (doPremult) {
                    dstPix[c] = PIX(float(dstPix[c]) * dstPix[3]);
                } else {
                    if (dstPix[3] != 0) {
                        dstPix[c] = PIX( dstPix[c] / float(dstPix[3]) );
                    }
                }
            }
        }
    }
}

template <bool doPremult>
void
Image::premultForDepth(const RectI& roi)
{
    if (getComponentsCount() != 4) {
        return;
    }
    ImageBitDepthEnum depth = getBitDepth();
    switch (depth) {
    case eImageBitDepthByte:
        premultInternal<unsigned char, doPremult>(roi);
        break;
    case eImageBitDepthShort:
        premultInternal<unsigned short, doPremult>(roi);
        break;
    case eImageBitDepthFloat:
        premultInternal<float, doPremult>(roi);
        break;
    default:
        break;
    }
}

void
Image::premultImage(const RectI& roi)
{
    assert(getStorageMode() != eStorageModeGLTex);
    premultForDepth<true>(roi);
}

void
Image::unpremultImage(const RectI& roi)
{
    assert(getStorageMode() != eStorageModeGLTex);
    premultForDepth<false>(roi);
}

NATRON_NAMESPACE_EXIT;
