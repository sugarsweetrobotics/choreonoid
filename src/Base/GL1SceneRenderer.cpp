/*!
  @file
  @author Shin'ichiro Nakaoka
*/

#include "GL1SceneRenderer.h"
#include <cnoid/SceneDrawables>
#include <cnoid/SceneCameras>
#include <cnoid/SceneLights>
#include <cnoid/SceneEffects>
#include <cnoid/EigenUtil>
#include <cnoid/NullOut>
#include <Eigen/StdVector>
#ifdef _WIN32
#include <Windows.h>
#endif
#include <GL/glew.h>
#ifdef _WIN32
#include <GL/wglew.h>
#endif
#include <boost/unordered_map.hpp>
#include <boost/dynamic_bitset.hpp>
#include <boost/bind.hpp>
#include <boost/scoped_ptr.hpp>
#include <iostream>

using namespace std;
using namespace cnoid;

namespace {

const bool USE_DISPLAY_LISTS = true;
const bool USE_VBO = false;
const bool USE_INDEXING = false;
const bool SHOW_IMAGE_FOR_PICKING = false;

const float MinLineWidthForPicking = 5.0f;

typedef vector<Affine3, Eigen::aligned_allocator<Affine3> > Affine3Array;

struct TransparentShapeInfo
{
    EIGEN_MAKE_ALIGNED_OPERATOR_NEW
    SgShape* shape;
    unsigned int pickId;
    Affine3 V; // view matrix
};
typedef boost::shared_ptr<TransparentShapeInfo> TransparentShapeInfoPtr;

    
/*
  A set of variables associated with a scene node
*/
class ShapeCache : public Referenced
{
public:
    GLuint listID;
    GLuint listIDforPicking;
    bool useIDforPicking;
    GLuint bufferNames[4];
    GLuint size;
    vector<TransparentShapeInfoPtr> transparentShapes;
        
    ShapeCache() {
        listID = 0;
        useIDforPicking = false;
        listIDforPicking = 0;
        for(int i=0; i < 4; ++i){
            bufferNames[i] = GL_INVALID_VALUE;
        }
    }
    ~ShapeCache() {
        if(listID){
            glDeleteLists(listID, 1);
        }
        if(listIDforPicking){
            glDeleteLists(listIDforPicking, 1);
        }
        for(int i=0; i < 4; ++i){
            if(bufferNames[i] != GL_INVALID_VALUE){
                glDeleteBuffers(1, &bufferNames[i]);
            }
        }
    }
    GLuint& vertexBufferName() { return bufferNames[0]; }
    GLuint& normalBufferName() { return bufferNames[1]; }
    GLuint& indexBufferName() { return bufferNames[2]; }
    GLuint& texCoordBufferName() { return bufferNames[3]; }
};
typedef ref_ptr<ShapeCache> ShapeCachePtr;


class TextureCache : public Referenced
{
public:
    bool isBound;
    bool isImageUpdateNeeded;
    GLuint textureName;
    int width;
    int height;
    int numComponents;
        
    TextureCache(){
        isBound = false;
        isImageUpdateNeeded = true;
        width = 0;
        height = 0;
        numComponents = 0;
    }
    bool isSameSizeAs(const Image& image){
        return (width == image.width() && height == image.height() && numComponents == image.numComponents());
    }
            
    ~TextureCache(){
        if(isBound){
            glDeleteTextures(1, &textureName);
        }
    }
};
typedef ref_ptr<TextureCache> TextureCachePtr;


struct SgObjectPtrHash {
    std::size_t operator()(const SgObjectPtr& p) const {
        return boost::hash_value<SgObject*>(p.get());
    }
};
typedef boost::unordered_map<SgObjectPtr, ReferencedPtr, SgObjectPtrHash> CacheMap;

}

namespace cnoid {

class GL1SceneRendererImpl
{
public:
    EIGEN_MAKE_ALIGNED_OPERATOR_NEW
        
    GL1SceneRenderer* self;

    Affine3Array Vstack; // stack of the model/view matrices

    typedef vector<Vector4f, Eigen::aligned_allocator<Vector4f> > ColorArray;
        
    struct Buf {
        EIGEN_MAKE_ALIGNED_OPERATOR_NEW
        SgVertexArray vertices;
        SgIndexArray triangles;
        SgNormalArray normals;
        ColorArray colors;
        SgTexCoordArray texCoords;
    };
    boost::scoped_ptr<Buf> buf;
        
    CacheMap cacheMaps[2];
    bool doUnusedCacheCheck;
    bool isCheckingUnusedCaches;
    bool hasValidNextCacheMap;
    bool isCacheClearRequested;
    int currentCacheMapIndex;
    CacheMap* currentCacheMap;
    CacheMap* nextCacheMap;
    ShapeCache* currentShapeCache;

    int currentShapeCacheTopViewMatrixIndex;

    Affine3 lastViewMatrix;
    Matrix4 lastProjectionMatrix;
    Affine3 currentModelTransform;

    int numSystemLights;
    int prevNumLights;
    bool defaultLighting;
    bool isHeadLightLightingFromBackEnabled;

    bool isCurrentFogUpdated;
    SgFogPtr prevFog;
    ScopedConnection currentFogConnection;

    GLint maxLights;
    bool defaultSmoothShading;
    bool isTextureEnabled;
    SgMaterialPtr defaultMaterial;
    GLfloat defaultPointSize;
    GLfloat defaultLineWidth;
    GLuint defaultTextureName;
        
    bool doNormalVisualization;
    double normalLength;

    bool isCompiling;
    bool isNewDisplayListDoubleRenderingEnabled;
    bool isNewDisplayListCreated;
    bool isPicking;

    GLdouble pickX;
    GLdouble pickY;
    typedef boost::shared_ptr<SgNodePath> SgNodePathPtr;
    SgNodePath currentNodePath;
    vector<SgNodePathPtr> pickingNodePathList;
    SgNodePath pickedNodePath;
    Vector3 pickedPoint;

    vector<TransparentShapeInfoPtr> transparentShapeInfos;

    // OpenGL states
    enum StateFlag {
        CURRENT_COLOR,
        COLOR_MATERIAL,
        DIFFUSE_COLOR,
        AMBIENT_COLOR,
        EMISSION_COLOR,
        SPECULAR_COLOR,
        SHININESS,
        CULL_FACE,
        CCW,
        LIGHTING,
        LIGHT_MODEL_TWO_SIDE,
        BLEND,
        DEPTH_MASK,
        POINT_SIZE,
        LINE_WIDTH,
        NUM_STATE_FLAGS
    };

    boost::dynamic_bitset<> stateFlag;
        
    Vector4f currentColor;
    Vector4f diffuseColor;
    Vector4f ambientColor;
    Vector4f emissionColor;
    Vector4f specularColor;
    float shininess;
    float lastAlpha;
    bool isColorMaterialEnabled;
    bool isCullFaceEnabled;
    bool isCCW;
    bool isLightingEnabled;
    bool isLightModelTwoSide;
    bool isBlendEnabled;
    bool isDepthMaskEnabled;
    float pointSize;
    float lineWidth;

    ostream* os_;
    ostream& os() { return *os_; }

    GL1SceneRendererImpl(GL1SceneRenderer* self);
    ~GL1SceneRendererImpl();
    bool initializeGL();
    void beginRendering(bool doRenderingCommands);
    void beginActualRendering(SgCamera* camera);
    void renderCamera(SgCamera* camera, const Affine3& cameraPosition);
    void renderLights(const Affine3& cameraPosition);
    void renderLight(const SgLight* light, GLint id, const Affine3& T);
    void renderFog();
    void onCurrentFogNodeUdpated();
    void endRendering();
    void render();
    bool pick(int x, int y);
    inline void setPickColor(unsigned int id);
    inline unsigned int pushPickName(SgNode* node, bool doSetColor = true);
    void popPickName();
    void visitInvariantGroup(SgInvariantGroup* group);
    void visitShape(SgShape* shape);
    void visitPointSet(SgPointSet* pointSet);
    void renderPlot(SgPlot* plot, SgVertexArray& expandedVertices, GLenum primitiveMode);
    void visitLineSet(SgLineSet* lineSet);
    void renderMaterial(const SgMaterial* material);
    bool renderTexture(SgTexture* texture, bool withMaterial);
    void putMeshData(SgMesh* mesh);
    void renderMesh(SgMesh* mesh, bool hasTexture);
    void renderTransparentShapes();
    void writeVertexBuffers(SgMesh* mesh, ShapeCache* cache, bool hasTexture);
    void visitOutlineGroup(SgOutlineGroup* outline);

    void clearGLState();
    void setColor(const Vector4f& color);
    void enableColorMaterial(bool on);
    void setDiffuseColor(const Vector4f& color);
    void setAmbientColor(const Vector4f& color);
    void setEmissionColor(const Vector4f& color);
    void setSpecularColor(const Vector4f& color);
    void setShininess(float shininess);
    void enableCullFace(bool on);
    void setFrontCCW(bool on);
    void enableLighting(bool on);
    void setLightModelTwoSide(bool on);
    void enableBlend(bool on);
    void enableDepthMask(bool on);
    void setPointSize(float size);
    void setLineWidth(float width);
    void getCurrentCameraTransform(Affine3& T);

    Vector4f createColorWithAlpha(const Vector3f& c3){
        Vector4f c4;
        c4.head<3>() = c3;
        c4[3] = lastAlpha;
        return c4;
    }
};

}


GL1SceneRenderer::GL1SceneRenderer()
{
    impl = new GL1SceneRendererImpl(this);
}


GL1SceneRenderer::GL1SceneRenderer(SgGroup* sceneRoot)
    : GLSceneRenderer(sceneRoot)
{
    impl = new GL1SceneRendererImpl(this);
}


GL1SceneRendererImpl::GL1SceneRendererImpl(GL1SceneRenderer* self)
    : self(self)
{
    Vstack.reserve(16);
    
    buf.reset(new Buf);

    doUnusedCacheCheck = true;
    currentCacheMapIndex = 0;
    hasValidNextCacheMap = false;
    isCacheClearRequested = false;
    currentCacheMap = &cacheMaps[0];
    nextCacheMap = &cacheMaps[1];

    lastViewMatrix.setIdentity();
    lastProjectionMatrix.setIdentity();

    numSystemLights = 2;
    maxLights = numSystemLights;
    prevNumLights = 0;
    isHeadLightLightingFromBackEnabled = false;

    prevFog = 0;

    defaultLighting = true;
    defaultSmoothShading = true;
    defaultMaterial = new SgMaterial;
    isTextureEnabled = true;
    defaultPointSize = 1.0f;
    defaultLineWidth = 1.0f;
    
    doNormalVisualization = false;
    normalLength = 0.0;

    isCompiling = false;
    isNewDisplayListDoubleRenderingEnabled = false;
    isNewDisplayListCreated = false;
    isPicking = false;
    pickedPoint.setZero();

    stateFlag.resize(NUM_STATE_FLAGS, false);
    clearGLState();

    os_ = &nullout();
}


GL1SceneRenderer::~GL1SceneRenderer()
{
    delete impl;
}


GL1SceneRendererImpl::~GL1SceneRendererImpl()
{

}


void GL1SceneRenderer::setOutputStream(std::ostream& os)
{
    impl->os_ = &os;
}


bool GL1SceneRenderer::initializeGL()
{
    GLSceneRenderer::initializeGL();
    return impl->initializeGL();
}


bool GL1SceneRendererImpl::initializeGL()
{
    GLenum err = glewInit();
    if(err != GLEW_OK){
        return false;
    }

    isCullFaceEnabled = false;
    if(isCullFaceEnabled){
        glEnable(GL_CULL_FACE);
        GLint twoSide = 0;
        isLightModelTwoSide = false;
        glLightModeliv(GL_LIGHT_MODEL_TWO_SIDE, &twoSide);
    } else {
        glDisable(GL_CULL_FACE);
        GLint twoSide = 1;
        isLightModelTwoSide = true;
        glLightModeliv(GL_LIGHT_MODEL_TWO_SIDE, &twoSide);
    }
    
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_NORMALIZE);
    setFrontCCW(true);

    glGetIntegerv(GL_MAX_LIGHTS, &maxLights);
    
    glDisable(GL_FOG);
    isCurrentFogUpdated = false;

    glGenTextures(1, &defaultTextureName);

    return true;
}


void GL1SceneRenderer::requestToClearCache()
{
    impl->isCacheClearRequested = true;
}


void GL1SceneRenderer::initializeRendering()
{
    impl->beginRendering(false);
}


void GL1SceneRenderer::beginRendering()
{
    impl->beginRendering(true);
}


void GL1SceneRendererImpl::beginRendering(bool doRenderingCommands)
{
    isCheckingUnusedCaches = isPicking ? false : doUnusedCacheCheck;

    if(isCacheClearRequested){
        cacheMaps[0].clear();
        cacheMaps[1].clear();
        hasValidNextCacheMap = false;
        isCheckingUnusedCaches = false;
        isCacheClearRequested = false;
    }
    if(hasValidNextCacheMap){
        currentCacheMapIndex = 1 - currentCacheMapIndex;
        currentCacheMap = &cacheMaps[currentCacheMapIndex];
        nextCacheMap = &cacheMaps[1 - currentCacheMapIndex];
        hasValidNextCacheMap = false;
    }
    currentShapeCache = 0;

    if(doRenderingCommands){
        if(isPicking){
            currentNodePath.clear();
            pickingNodePathList.clear();
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        } else {
            const Vector3f& c = self->backgroundColor();
            glClearColor(c[0], c[1], c[2], 1.0f);
        }
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    }

    self->extractPreproNodes();

    if(doRenderingCommands){
        SgCamera* camera = self->currentCamera();
        if(camera){
            beginActualRendering(camera);
        }
    }
}


void GL1SceneRendererImpl::beginActualRendering(SgCamera* camera)
{
    const Affine3& cameraPosition = self->currentCameraPosition();
    
    renderCamera(camera, cameraPosition);

    if(isPicking || !defaultLighting){
        glDisable(GL_LIGHTING);
    } else {
        renderLights(cameraPosition);
        renderFog();
    }

    if(isPicking){
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    } else {
        switch(self->polygonMode()){
        case GLSceneRenderer::FILL_MODE:
            glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
            break;
        case GLSceneRenderer::LINE_MODE:
            glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
            break;
        case GLSceneRenderer::POINT_MODE:
            glPolygonMode(GL_FRONT_AND_BACK, GL_POINT);
            break;
        }
    }

    if(defaultSmoothShading){
        glShadeModel(GL_SMOOTH);
    } else {
        glShadeModel(GL_FLAT);
    }

    isNewDisplayListCreated = false;
            
    clearGLState();
    
    setColor(self->defaultColor());
    setPointSize(defaultPointSize);
    //glEnable(GL_POINT_SMOOTH);
    setLineWidth(defaultLineWidth);
    //glEnable(GL_LINE_SMOOTH);
    
    transparentShapeInfos.clear();
}


void GL1SceneRendererImpl::renderCamera(SgCamera* camera, const Affine3& cameraPosition)
{
    // set projection
    if(SgPerspectiveCamera* pers = dynamic_cast<SgPerspectiveCamera*>(camera)){
        double aspectRatio = self->aspectRatio();
        self->getPerspectiveProjectionMatrix(
            pers->fovy(aspectRatio), aspectRatio, pers->nearDistance(), pers->farDistance(),
            lastProjectionMatrix);
        
    } else if(SgOrthographicCamera* ortho = dynamic_cast<SgOrthographicCamera*>(camera)){
        GLfloat left, right, bottom, top;
        self->getViewVolume(ortho, left, right, bottom, top);
        self->getOrthographicProjectionMatrix(
            left, right, bottom, top, ortho->nearDistance(), ortho->farDistance(),
            lastProjectionMatrix);
        
    } else {
        self->getPerspectiveProjectionMatrix(
            radian(40.0), self->aspectRatio(), 0.01, 1.0e4,
            lastProjectionMatrix);
    }

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glMultMatrixd(lastProjectionMatrix.data());

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    
    Vstack.clear();
    Vstack.push_back(cameraPosition.inverse(Eigen::Isometry));
    const Affine3& V = Vstack.back();
    lastViewMatrix = V;
    glLoadMatrixd(V.data());
}


void GL1SceneRendererImpl::renderLights(const Affine3& cameraPosition)
{
    glEnable(GL_LIGHTING);

    SgLight* headLight = self->headLight();
    if(!headLight->on()){
        glDisable(GL_LIGHT0);
        glDisable(GL_LIGHT1);
    } else {
        renderLight(headLight, GL_LIGHT0, cameraPosition);
        if(isHeadLightLightingFromBackEnabled){
            if(SgDirectionalLight* directionalHeadLight = dynamic_cast<SgDirectionalLight*>(headLight)){
                SgDirectionalLight lightFromBack(*directionalHeadLight);
                lightFromBack.setDirection(-directionalHeadLight->direction());
                renderLight(&lightFromBack, GL_LIGHT1, cameraPosition);
            }
        }
    }
    
    const int numLights = std::min(self->numLights(), (int)(maxLights - numSystemLights));

    for(size_t i=0; i < numLights; ++i){
        SgLight* light;
        Affine3 T;
        self->getLightInfo(i, light, T);
        const GLint id = GL_LIGHT0 + numSystemLights + i;
        renderLight(light, id, T);
    }

    for(size_t i = numLights; i < prevNumLights; ++i){
        const GLint lightID = GL_LIGHT0 + numSystemLights + i;
        glDisable(lightID);
    }

    prevNumLights = numLights;
}


void GL1SceneRendererImpl::renderLight(const SgLight* light, GLint id, const Affine3& T)
{
    bool isValid = false;

    if(light->on()){
        
        if(const SgDirectionalLight* dirLight = dynamic_cast<const SgDirectionalLight*>(light)){

            isValid = true;
            
            Vector4f pos;
            pos << (T.linear() * -dirLight->direction()).cast<float>(), 0.0f;
            glLightfv(id, GL_POSITION, pos.data());
            
            /*
              glLightf(id, GL_CONSTANT_ATTENUATION, 0.0f);
              glLightf(id, GL_LINEAR_ATTENUATION, 0.0f);
              glLightf(id, GL_QUADRATIC_ATTENUATION, 0.0f);
            */
            
        } else if(const SgPointLight* pointLight = dynamic_cast<const SgPointLight*>(light)){
            
            isValid = true;
            
            Vector4f pos;
            pos << T.translation().cast<float>(), 1.0f;
            glLightfv(id, GL_POSITION, pos.data());
            
            glLightf(id, GL_CONSTANT_ATTENUATION, pointLight->constantAttenuation());
            glLightf(id, GL_LINEAR_ATTENUATION, pointLight->linearAttenuation());
            glLightf(id, GL_QUADRATIC_ATTENUATION, pointLight->quadraticAttenuation());
            
            if(const SgSpotLight* spotLight = dynamic_cast<const SgSpotLight*>(pointLight)){
                Vector3f direction = (T.linear() * spotLight->direction()).cast<GLfloat>();
                glLightfv(id, GL_SPOT_DIRECTION, direction.data());
                glLightf(id, GL_SPOT_CUTOFF, degree(spotLight->cutOffAngle()));
                glLightf(id, GL_SPOT_EXPONENT, 0.5f);
                
            } else {
                glLightf(id, GL_SPOT_CUTOFF, 180.0f);
            }
        }
    }
        
    if(!isValid){
        glDisable(id);
        
    } else {
        Vector4f diffuse;
        diffuse << (light->intensity() * light->color()), 1.0f;
        glLightfv(id, GL_DIFFUSE, diffuse.data());
        glLightfv(id, GL_SPECULAR, diffuse.data());
        
        Vector4f ambient;
        ambient << (light->ambientIntensity() * light->color()), 1.0f;
        glLightfv(id, GL_AMBIENT, ambient.data());
        
        glEnable(id);
    }
}


void GL1SceneRendererImpl::renderFog()
{
    SgFog* fog = 0;
    if(self->isFogEnabled()){
        int n = self->numFogs();
        if(n > 0){
            fog = self->fog(n - 1); // use the last fog
        }
    }
    if(fog != prevFog){
        isCurrentFogUpdated = true;
        if(!fog){
            currentFogConnection.disconnect();
        } else {
            currentFogConnection.reset(
                fog->sigUpdated().connect(
                    boost::bind(&GL1SceneRendererImpl::onCurrentFogNodeUdpated, this)));
        }
    }

    if(isCurrentFogUpdated){
        if(!fog){
            glDisable(GL_FOG);
        } else {
            glEnable(GL_FOG);
            GLfloat color[4];
            const Vector3f& c = fog->color();
            color[0] = c[0];
            color[1] = c[1];
            color[2] = c[2];
            color[3] = 1.0f;
            glFogfv(GL_FOG_COLOR, color);
            glFogi(GL_FOG_MODE, GL_LINEAR);
            glFogi(GL_FOG_HINT, GL_FASTEST);
            glFogf(GL_FOG_START, 0.0f);
            glFogf(GL_FOG_END, fog->visibilityRange());
        }
    }
    isCurrentFogUpdated = false;
    prevFog = fog;
}


void GL1SceneRendererImpl::onCurrentFogNodeUdpated()
{
    isCurrentFogUpdated = true;
}


void GL1SceneRenderer::endRendering()
{
    impl->endRendering();
}


void GL1SceneRendererImpl::endRendering()
{
    if(isCheckingUnusedCaches){
        currentCacheMap->clear();
        hasValidNextCacheMap = true;
    }

    if(isNewDisplayListDoubleRenderingEnabled && isNewDisplayListCreated){
        self->scene()->notifyUpdate();
    }
}


void GL1SceneRenderer::render()
{
    impl->render();
}


void GL1SceneRendererImpl::render()
{
    beginRendering(true);

    self->sceneRoot()->accept(*self);

    if(!transparentShapeInfos.empty()){
        renderTransparentShapes();
    }

    endRendering();
}


bool GL1SceneRenderer::pick(int x, int y)
{
    return impl->pick(x, y);
}


/*
  http://stackoverflow.com/questions/4040616/opengl-gl-select-or-manual-collision-detection
  http://content.gpwiki.org/index.php/OpenGL_Selection_Using_Unique_Color_IDs
  http://www.opengl-tutorial.org/miscellaneous/clicking-on-objects/picking-with-an-opengl-hack/
  http://www.codeproject.com/Articles/35139/Interactive-Techniques-in-Three-dimensional-Scenes#_OpenGL_Picking_by
  http://en.wikibooks.org/wiki/OpenGL_Programming/Object_selection
*/
bool GL1SceneRendererImpl::pick(int x, int y)
{
    glPushAttrib(GL_ENABLE_BIT);

    glDisable(GL_BLEND);
    glDisable(GL_MULTISAMPLE);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_DITHER);
    glDisable(GL_FOG);

    if(!SHOW_IMAGE_FOR_PICKING){
        glScissor(x, y, 1, 1);
        glEnable(GL_SCISSOR_TEST);
    }
    
    isPicking = true;
    render();
    isPicking = false;

    glPopAttrib();

    GLfloat color[4];
    glReadPixels(x, y, 1, 1, GL_RGBA, GL_FLOAT, color);
    if(SHOW_IMAGE_FOR_PICKING){
        color[2] = 0.0f;
    }
    unsigned int id = (color[0] * 255) + ((int)(color[1] * 255) << 8) + ((int)(color[2] * 255) << 16) - 1;

    pickedNodePath.clear();

    if(0 < id && id < pickingNodePathList.size()){
        GLfloat depth;
        glReadPixels(x, y, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &depth);
        Vector3 projected;
        if(self->unproject(x, y, depth, pickedPoint)){
            pickedNodePath = *pickingNodePathList[id];
        }
    }

    return !pickedNodePath.empty();
}


const std::vector<SgNode*>& GL1SceneRenderer::pickedNodePath() const
{
    return impl->pickedNodePath;
}


const Vector3& GL1SceneRenderer::pickedPoint() const
{
    return impl->pickedPoint;
}



inline void GL1SceneRendererImpl::setPickColor(unsigned int id)
{
    float r = (id & 0xff) / 255.0;
    float g = ((id >> 8) & 0xff) / 255.0;
    float b = ((id >> 16) & 0xff) / 255.0;
    if(SHOW_IMAGE_FOR_PICKING){
        b = 1.0f;
    }
    glColor4f(r, g, b, 1.0f);
    currentColor << r, g, b, 1.0f;
}
        

/**
   @return id of the current object
*/
inline unsigned int GL1SceneRendererImpl::pushPickName(SgNode* node, bool doSetColor)
{
    unsigned int id = 0;
    
    if(isPicking && !isCompiling){
        id = pickingNodePathList.size() + 1;
        currentNodePath.push_back(node);
        pickingNodePathList.push_back(boost::make_shared<SgNodePath>(currentNodePath));
        if(doSetColor){
            setPickColor(id);
        }
    }

    return id;
}


inline void GL1SceneRendererImpl::popPickName()
{
    if(isPicking && !isCompiling){
        currentNodePath.pop_back();
    }
}


void GL1SceneRenderer::visitGroup(SgGroup* group)
{
    impl->pushPickName(group);
    SceneVisitor::visitGroup(group);
    impl->popPickName();
}


void GL1SceneRenderer::visitInvariantGroup(SgInvariantGroup* group)
{
    impl->visitInvariantGroup(group);
}


void GL1SceneRendererImpl::visitInvariantGroup(SgInvariantGroup* group)
{
    if(!USE_DISPLAY_LISTS || isCompiling){
        self->visitGroup(group);

    } else {
        ShapeCache* cache;
        CacheMap::iterator p = currentCacheMap->find(group);
        if(p == currentCacheMap->end()){
            cache = new ShapeCache();
            currentCacheMap->insert(CacheMap::value_type(group, cache));
        } else {
            cache = static_cast<ShapeCache*>(p->second.get());
        }

        if(!cache->listID && !isPicking){
            currentShapeCache = cache;
            currentShapeCacheTopViewMatrixIndex = Vstack.size() - 1;

            cache->listID = glGenLists(1);
            if(cache->listID){
                glNewList(cache->listID, GL_COMPILE);

                isCompiling = true;
                clearGLState();
                self->visitGroup(group);
                isCompiling = false;

                if(stateFlag[LIGHTING] || stateFlag[CURRENT_COLOR]){
                    cache->useIDforPicking = true;
                }
                glEndList();

                isNewDisplayListCreated = true;
            }
        }

        GLuint listID = cache->listID;

        if(listID){
            if(isPicking && cache->useIDforPicking){
                if(!cache->listIDforPicking){
                    currentShapeCache = cache;
                    currentShapeCacheTopViewMatrixIndex = Vstack.size() - 1;
                    cache->listIDforPicking = glGenLists(1);
                    if(cache->listIDforPicking){
                        glNewList(cache->listIDforPicking, GL_COMPILE);
                        isCompiling = true;
                        clearGLState();
                        self->visitGroup(group);
                        isCompiling = false;
                        glEndList();
                    }
                }
                listID = cache->listIDforPicking;
            }
            if(listID){
                const unsigned int pickId = pushPickName(group);
                glPushAttrib(GL_ENABLE_BIT);
                glCallList(listID);
                glPopAttrib();
                clearGLState();
                popPickName();

                const vector<TransparentShapeInfoPtr>& transparentShapes = cache->transparentShapes;
                if(!transparentShapes.empty()){
                    const Affine3& V = Vstack.back();
                    for(size_t i=0; i < transparentShapes.size(); ++i){
                        const TransparentShapeInfo& src = *transparentShapes[i];
                        TransparentShapeInfoPtr info = make_shared_aligned<TransparentShapeInfo>();
                        info->shape = src.shape;
                        info->pickId = pickId;
                        info->V = V * src.V;
                        transparentShapeInfos.push_back(info);
                    }
                }
            }

            if(isCheckingUnusedCaches){
                nextCacheMap->insert(CacheMap::value_type(group, cache));
            }
        }
    }
    currentShapeCache = 0;
}


void GL1SceneRenderer::visitTransform(SgTransform* transform)
{
    Affine3 T;
    transform->getTransform(T);

    Affine3Array& Vstack = impl->Vstack;
    Vstack.push_back(Vstack.back() * T);

    glPushMatrix();
    glMultMatrixd(T.data());

    /*
      if(isNotRotationMatrix){
      glPushAttrib(GL_ENABLE_BIT);
      glEnable(GL_NORMALIZE);
      }
    */
    
    visitGroup(transform);
    
    /*
      if(isNotRotationMatrix){
      glPopAttrib();
      }
    */
    
    glPopMatrix();
    Vstack.pop_back();
}


void GL1SceneRendererImpl::visitShape(SgShape* shape)
{
    SgMesh* mesh = shape->mesh();
    if(mesh){
        if(mesh->hasVertices()){
            SgMaterial* material = shape->material();
            SgTexture* texture = isTextureEnabled ? shape->texture() : 0;

            if((material && material->transparency() > 0.0)
               /* || (texture && texture->constImage().hasAlphaComponent()) */){
                // A transparent shape is rendered later
                TransparentShapeInfoPtr info = make_shared_aligned<TransparentShapeInfo>();
                info->shape = shape;
                if(isCompiling){
                    info->V = Vstack[currentShapeCacheTopViewMatrixIndex].inverse() * Vstack.back();
                    currentShapeCache->transparentShapes.push_back(info);
                } else {
                    info->V = Vstack.back();
                    info->pickId = pushPickName(shape, false);
                    popPickName();
                    transparentShapeInfos.push_back(info);
                }
            } else {
                pushPickName(shape);
                bool hasTexture = false;
                if(!isPicking){
                    renderMaterial(material);
                    if(texture && mesh->hasTexCoords()){
                        hasTexture = renderTexture(texture, material);
                    }
                }
                renderMesh(mesh, hasTexture);
                popPickName();
            }
        }
    }
}


void GL1SceneRenderer::visitUnpickableGroup(SgUnpickableGroup* group)
{
    if(!impl->isPicking){
        visitGroup(group);
    }
}


void GL1SceneRenderer::visitShape(SgShape* shape)
{
    impl->visitShape(shape);
}


void GL1SceneRendererImpl::renderMaterial(const SgMaterial* material)
{
    if(!material){
        material = defaultMaterial;
    }
    
    float alpha = 1.0 - material->transparency();

    Vector4f color;
    color << material->diffuseColor(), alpha;
    setDiffuseColor(color);
        
    color.head<3>() *= material->ambientIntensity();
    setAmbientColor(color);

    color << material->emissiveColor(), alpha;
    setEmissionColor(color);
    
    color << material->specularColor(), alpha;
    setSpecularColor(color);
    
    float shininess = (127.0 * material->shininess()) + 1.0;
    setShininess(shininess);

    lastAlpha = alpha;
}


bool GL1SceneRendererImpl::renderTexture(SgTexture* texture, bool withMaterial)
{
    SgImage* sgImage = texture->image();
    if(!sgImage || sgImage->empty()){
        return false;
    }

    const Image& image = sgImage->constImage();
    const int width = image.width();
    const int height = image.height();
    bool doLoadTexImage = false;
    bool doReloadTexImage = false;

    CacheMap::iterator p = currentCacheMap->find(sgImage);
    TextureCache* cache;
    if(p != currentCacheMap->end()){
        cache = static_cast<TextureCache*>(p->second.get());
    } else {
        cache = new TextureCache;
        currentCacheMap->insert(CacheMap::value_type(sgImage, cache));
    }
    if(cache->isBound){
        glBindTexture(GL_TEXTURE_2D, cache->textureName);
        if(cache->isImageUpdateNeeded){
            doLoadTexImage = true;
            doReloadTexImage = cache->isSameSizeAs(image);
        }
    } else {
        glGenTextures(1, &cache->textureName);
        glBindTexture(GL_TEXTURE_2D, cache->textureName);
        cache->isBound = true;
        doLoadTexImage = true;
    }
    if(isCheckingUnusedCaches){
        nextCacheMap->insert(CacheMap::value_type(sgImage, cache));
    }
    cache->width = width;
    cache->height = height;
    cache->numComponents = image.numComponents();
    cache->isImageUpdateNeeded = false;
    
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, texture->repeatS() ? GL_REPEAT : GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, texture->repeatT() ? GL_REPEAT : GL_CLAMP);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, withMaterial ? GL_MODULATE : GL_REPLACE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    if(isCompiling){
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_GENERATE_MIPMAP, GL_TRUE);
    } else {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    }

    if(doLoadTexImage){
        GLint internalFormat = GL_RGB;
        GLenum format = GL_RGB;
        
        switch(image.numComponents()){
        case 1 : internalFormat = GL_LUMINANCE;
            format = GL_LUMINANCE;
            break;
        case 2 : internalFormat = GL_LUMINANCE_ALPHA;
            format = GL_LUMINANCE_ALPHA;
            break;
        case 3 : internalFormat = GL_RGB;
            format = GL_RGB;
            break;
        case 4 : internalFormat = GL_RGBA;
            format = GL_RGBA;
            break;
        default :
            return false;
        }
        
        if(image.numComponents() == 3){
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        } else {
            glPixelStorei(GL_UNPACK_ALIGNMENT, image.numComponents());
        }

        if(!isCompiling && doReloadTexImage){
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, format, GL_UNSIGNED_BYTE, image.pixels());
        } else {
            glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, width, height, 0, format, GL_UNSIGNED_BYTE, image.pixels());            
        } 
    }

    if(SgTextureTransform* tt = texture->textureTransform()){
        glMatrixMode(GL_TEXTURE);
        glLoadIdentity();
        glTranslated(-tt->center()[0], -tt->center()[1], 0.0 );
        glScaled(tt->scale()[0], tt->scale()[1], 0.0 );
        glRotated(tt->rotation(), 0.0, 0.0, 1.0 );
        glTranslated(tt->center()[0], tt->center()[1], 0.0 );
        glTranslated(tt->translation()[0], tt->translation()[1], 0.0 );
        glMatrixMode(GL_MODELVIEW);
    } else {
        glMatrixMode(GL_TEXTURE);
        glLoadIdentity();
        glMatrixMode(GL_MODELVIEW);
    }

    return true;
}


void GL1SceneRenderer::onImageUpdated(SgImage* image)
{
    CacheMap* cacheMap = impl->hasValidNextCacheMap ? impl->nextCacheMap : impl->currentCacheMap;
    CacheMap::iterator p = cacheMap->find(image);
    if(p != cacheMap->end()){
        TextureCache* cache = static_cast<TextureCache*>(p->second.get());
        cache->isImageUpdateNeeded = true;
    }
}


/**
   \todo sort the shape nodes by the distance from the viewpoint
*/
void GL1SceneRendererImpl::renderTransparentShapes()
{
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();;
    
    if(!isPicking){
        enableBlend(true);
    }
    
    const int n = transparentShapeInfos.size();
    for(int i=0; i < n; ++i){
        TransparentShapeInfo& info = *transparentShapeInfos[i];
        glMatrixMode(GL_MODELVIEW);
        glLoadMatrixd(info.V.data());
        SgShape* shape = info.shape;
        bool hasTexture = false;
        if(isPicking){
            setPickColor(info.pickId);
        } else {
            renderMaterial(shape->material());
            SgTexture* texture = isTextureEnabled ? shape->texture() : 0;
            if(texture && shape->mesh()->hasTexCoords()){
                hasTexture = renderTexture(texture, shape->material());
            }
        }
        renderMesh(shape->mesh(), hasTexture);
    }

    if(!isPicking){
        enableBlend(false);
    }

    transparentShapeInfos.clear();

    glPopMatrix();
}


void GL1SceneRendererImpl::putMeshData(SgMesh* mesh)
{
    if(!mesh->hasColors()){
        return;
    }
        
    if(mesh->hasVertices()){
        cout << "vertices: \n";
        SgVertexArray& vertices = *mesh->vertices();
        for(size_t i=0; i < vertices.size(); ++i){
            const Vector3f& v = vertices[i];
            cout << "(" << v.x() << ", " << v.y() << ", " << v.z() << "), ";
        }
        cout << "\n";
    }
    if(!mesh->triangleVertices().empty()){
        cout << "triangles: \n";
        const int n = mesh->numTriangles();
        for(int i=0; i < n; ++i){
            SgMesh::TriangleRef t = mesh->triangle(i);
            cout << "(" << t[0] << ", " << t[1] << ", " << t[2] << "), ";
        }
        cout << "\n";
    }
    if(mesh->hasNormals()){
        cout << "normals: \n";
        SgNormalArray& normals = *mesh->normals();
        for(size_t i=0; i < normals.size(); ++i){
            const Vector3f& v = normals[i];
            cout << "(" << v.x() << ", " << v.y() << ", " << v.z() << "), ";
        }
        cout << "\n";
        SgIndexArray& indices = mesh->normalIndices();
        if(!indices.empty()){
            cout << "normalIndices: \n";
            for(size_t i=0; i < indices.size(); ++i){
                cout << indices[i] << ", ";
            }
            cout << "\n";
        }
    }
    if(mesh->hasColors()){
        cout << "colors: \n";
        SgColorArray& colors = *mesh->colors();
        for(size_t i=0; i < colors.size(); ++i){
            const Vector3f& c = colors[i];
            cout << "(" << c.x() << ", " << c.y() << ", " << c.z() << "), ";
        }
        cout << "\n";
        SgIndexArray& indices = mesh->colorIndices();
        if(!indices.empty()){
            cout << "colorIndices: \n";
            for(size_t i=0; i < indices.size(); ++i){
                cout << indices[i] << ", ";
            }
            cout << "\n";
        }
    }
    cout << endl;
}


void GL1SceneRendererImpl::renderMesh(SgMesh* mesh, bool hasTexture)
{
    if(false){
        putMeshData(mesh);
    }
    
    const bool ENABLE_CULLING = true;
    if(ENABLE_CULLING){
        enableCullFace(mesh->isSolid());
        setLightModelTwoSide(!mesh->isSolid());
    } else {
        enableCullFace(false);
        setLightModelTwoSide(true);
    }

    glPushClientAttrib(GL_CLIENT_VERTEX_ARRAY_BIT);

    if(!USE_VBO){
        writeVertexBuffers(mesh, 0, hasTexture);

    } else {
        ShapeCache* cache;
        CacheMap::iterator it = currentCacheMap->find(mesh);
        if(it != currentCacheMap->end()){
            cache = static_cast<ShapeCache*>(it->second.get());
        } else {
            it = currentCacheMap->insert(CacheMap::value_type(mesh, new ShapeCache)).first;
            cache = static_cast<ShapeCache*>(it->second.get());
            writeVertexBuffers(mesh, cache, hasTexture);
        }
        if(isCheckingUnusedCaches){
            nextCacheMap->insert(*it);
        }
        if(cache->vertexBufferName() != GL_INVALID_VALUE){
            glEnableClientState(GL_VERTEX_ARRAY);
            glBindBuffer(GL_ARRAY_BUFFER, cache->vertexBufferName());
            glVertexPointer(3, GL_FLOAT, 0, 0);
                        
            if(cache->normalBufferName() != GL_INVALID_VALUE){
                glEnableClientState(GL_NORMAL_ARRAY);
                glBindBuffer(GL_ARRAY_BUFFER, cache->normalBufferName());
                glNormalPointer(GL_FLOAT, 0, 0);
            }
            if(cache->texCoordBufferName() != GL_INVALID_VALUE){
                glEnableClientState(GL_TEXTURE_COORD_ARRAY);
                glBindBuffer(GL_ARRAY_BUFFER, cache->texCoordBufferName());
                glTexCoordPointer(2, GL_FLOAT, 0, 0);
                glEnable(GL_TEXTURE_2D);
            }
            if(USE_INDEXING){
                if(cache->indexBufferName() != GL_INVALID_VALUE){
                    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, cache->indexBufferName());
                    glDrawElements(GL_TRIANGLES, cache->size, GL_UNSIGNED_INT, 0);
                }
            } else {
                glDrawArrays(GL_TRIANGLES, 0, cache->size);
            }
            if(cache->texCoordBufferName() != GL_INVALID_VALUE){
                glDisable(GL_TEXTURE_2D);
            }
        }
    }

    glPopClientAttrib();
}


void GL1SceneRendererImpl::writeVertexBuffers(SgMesh* mesh, ShapeCache* cache, bool hasTexture)
{
    SgVertexArray& orgVertices = *mesh->vertices();
    SgIndexArray& orgTriangleVertices = mesh->triangleVertices();
    const size_t numTriangles = mesh->numTriangles();
    const size_t totalNumVertices = orgTriangleVertices.size();
    const bool hasNormals = mesh->hasNormals() && !isPicking;
    const bool hasColors = mesh->hasColors() && !isPicking;
    SgVertexArray* vertices = 0;
    SgNormalArray* normals = 0;
    SgIndexArray* triangleVertices = 0;
    ColorArray* colors = 0;
    SgTexCoordArray* texCoords = 0;

    if(USE_INDEXING){
        vertices = &orgVertices;
        triangleVertices = &orgTriangleVertices;
        if(hasNormals){
            if(mesh->normalIndices().empty() && mesh->normals()->size() == orgVertices.size()){
                normals = mesh->normals();
            } else {
                normals = &buf->normals;
                normals->resize(orgVertices.size());
            }
        }
        if(hasTexture){
            if(mesh->texCoordIndices().empty() && mesh->texCoords()->size() == orgVertices.size()){
                texCoords = mesh->texCoords();
            } else {
                texCoords = &buf->texCoords;
                texCoords->resize(orgVertices.size());
            }
        }
    } else {
        vertices = &buf->vertices;
        vertices->clear();
        vertices->reserve(totalNumVertices);
        if(hasNormals){
            normals = &buf->normals;
            normals->clear();
            normals->reserve(totalNumVertices);
        }
        if(hasColors){
            colors = &buf->colors;
            colors->clear();
            colors->reserve(totalNumVertices);
        }
        if(hasTexture){
            texCoords = &buf->texCoords;
            texCoords->clear();
            texCoords->reserve(totalNumVertices);
        }
    }
    
    int faceVertexIndex = 0;
    int numFaceVertices = 0;
    
    for(size_t i=0; i < numTriangles; ++i){
        for(size_t j=0; j < 3; ++j){
            const int orgVertexIndex = orgTriangleVertices[faceVertexIndex];
            if(!USE_INDEXING){
                vertices->push_back(orgVertices[orgVertexIndex]);
            }
            if(hasNormals){
                if(mesh->normalIndices().empty()){
                    if(!USE_INDEXING){
                        normals->push_back(mesh->normals()->at(orgVertexIndex));
                    }
                } else {
                    const int normalIndex = mesh->normalIndices()[faceVertexIndex];
                    if(USE_INDEXING){
                        normals->at(orgVertexIndex) = mesh->normals()->at(normalIndex);
                    } else {
                        normals->push_back(mesh->normals()->at(normalIndex));
                    }
                }
            }
            if(hasColors){
                if(mesh->colorIndices().empty()){
                    if(!USE_INDEXING){
                        colors->push_back(createColorWithAlpha(mesh->colors()->at(faceVertexIndex)));
                    }
                } else {
                    const int colorIndex = mesh->colorIndices()[faceVertexIndex];
                    if(USE_INDEXING){
                        colors->at(orgVertexIndex) = createColorWithAlpha(mesh->colors()->at(colorIndex));
                    } else {
                        colors->push_back(createColorWithAlpha(mesh->colors()->at(colorIndex)));
                    }
                }
            }
            if(hasTexture){
                if(mesh->texCoordIndices().empty()){
                    if(!USE_INDEXING){
                        texCoords->push_back(mesh->texCoords()->at(orgVertexIndex));
                    }
                }else{
                    const int texCoordIndex = mesh->texCoordIndices()[faceVertexIndex];
                    if(USE_INDEXING){
                        texCoords->at(orgVertexIndex) = mesh->texCoords()->at(texCoordIndex);
                    } else {
                        texCoords->push_back(mesh->texCoords()->at(texCoordIndex));
                    }
                }
            }
            ++faceVertexIndex;
        }
    }

    if(USE_VBO){
        if(cache->vertexBufferName() == GL_INVALID_VALUE){
            glGenBuffers(1, &cache->vertexBufferName());
            glBindBuffer(GL_ARRAY_BUFFER, cache->vertexBufferName());
            glBufferData(GL_ARRAY_BUFFER, vertices->size() * sizeof(Vector3f), vertices->data(), GL_STATIC_DRAW);
        }
    } else {
        glEnableClientState(GL_VERTEX_ARRAY);
        glVertexPointer(3, GL_FLOAT, 0, vertices->data());
    }
    if(normals){
        if(USE_VBO){
            if(cache->normalBufferName() == GL_INVALID_VALUE){
                glGenBuffers(1, &cache->normalBufferName());
                glBindBuffer(GL_ARRAY_BUFFER, cache->normalBufferName());
                glBufferData(GL_ARRAY_BUFFER, normals->size() * sizeof(Vector3f), normals->data(), GL_STATIC_DRAW);
            }
        } else {
            glEnableClientState(GL_NORMAL_ARRAY);
            glNormalPointer(GL_FLOAT, 0, normals->data());
        }
    }
    bool useColorArray = false;
    if(colors){
        if(!USE_VBO){
            glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
            glEnable(GL_COLOR_MATERIAL);
            glEnableClientState(GL_COLOR_ARRAY);
            glColorPointer(4, GL_FLOAT, 0, &colors[0][0]);
            useColorArray = true;
        }
    }
    if(hasTexture){
        if(USE_VBO){
            if(cache->texCoordBufferName() == GL_INVALID_VALUE){
                glGenBuffers(1, &cache->texCoordBufferName());
                glBindBuffer(GL_ARRAY_BUFFER, cache->texCoordBufferName());
                glBufferData(GL_ARRAY_BUFFER, texCoords->size() * sizeof(Vector2f), texCoords->data(), GL_STATIC_DRAW);
            }
        } else {
            glEnableClientState(GL_TEXTURE_COORD_ARRAY);
            glTexCoordPointer(2, GL_FLOAT, 0, texCoords->data());
        }
        glEnable(GL_TEXTURE_2D);
    }

    if(USE_VBO){
        if(!triangleVertices){
            cache->size = vertices->size();

        } else if(cache->indexBufferName() == GL_INVALID_VALUE){
            glGenBuffers(1, &cache->indexBufferName());
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, cache->indexBufferName());
            glBufferData(GL_ELEMENT_ARRAY_BUFFER, triangleVertices->size(), &triangleVertices->front(), GL_STATIC_DRAW);
            cache->size = triangleVertices->size();
        }
            
    } else {
        if(USE_INDEXING){
            glDrawElements(GL_TRIANGLES, triangleVertices->size(), GL_UNSIGNED_INT, &triangleVertices->front());
        } else {
            glDrawArrays(GL_TRIANGLES, 0, vertices->size());
        }
    }

    if(useColorArray){
        glDisable(GL_COLOR_MATERIAL);
        stateFlag.set(CURRENT_COLOR);
    }
    if(hasTexture){
        glDisable(GL_TEXTURE_2D);
    }

    if(doNormalVisualization && !isPicking){
        enableLighting(false);
        if(!USE_INDEXING){
            vector<Vector3f> lines;
            for(size_t i=0; i < vertices->size(); ++i){
                const Vector3f& v = (*vertices)[i];
                lines.push_back(v);
                lines.push_back(v + (*normals)[i] * normalLength);
            }
            glDisableClientState(GL_NORMAL_ARRAY);
            glVertexPointer(3, GL_FLOAT, 0, lines.front().data());
            setColor(Vector4f(0.0f, 1.0f, 0.0f, 1.0f));
            glDrawArrays(GL_LINES, 0, lines.size());
        }
        enableLighting(true);
    }
}


void GL1SceneRenderer::visitPointSet(SgPointSet* pointSet)
{
    impl->visitPointSet(pointSet);
}


void GL1SceneRendererImpl::visitPointSet(SgPointSet* pointSet)
{
    if(!pointSet->hasVertices()){
        return;
    }
    const double s = pointSet->pointSize();
    if(s > 0.0){
        setPointSize(s);
    }
    renderPlot(pointSet, *pointSet->vertices(), (GLenum)GL_POINTS);
    if(s > 0.0){
        setPointSize(s);
    }
}


void GL1SceneRendererImpl::renderPlot(SgPlot* plot, SgVertexArray& expandedVertices, GLenum primitiveMode)
{
    glPushClientAttrib(GL_CLIENT_VERTEX_ARRAY_BIT);
        
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(3, GL_FLOAT, 0, expandedVertices.data());
    
    SgMaterial* material = plot->material() ? plot->material() : defaultMaterial.get();
    
    if(!plot->hasNormals()){
        enableLighting(false);
        //glDisableClientState(GL_NORMAL_ARRAY);
        lastAlpha = 1.0;
        if(!plot->hasColors()){
            setColor(createColorWithAlpha(material->diffuseColor()));
        }
    } else if(!isPicking){
        enableCullFace(false);
        setLightModelTwoSide(true);
        renderMaterial(material);
        
        const SgNormalArray& orgNormals = *plot->normals();
        SgNormalArray& normals = buf->normals;
        const SgIndexArray& normalIndices = plot->normalIndices();
        if(normalIndices.empty()){
            normals = orgNormals;
        } else {
            normals.clear();
            normals.reserve(normalIndices.size());
            for(int i=0; i < normalIndices.size(); ++i){
                normals.push_back(orgNormals[normalIndices[i]]);
            }
        }
        glEnableClientState(GL_NORMAL_ARRAY);
        glNormalPointer(GL_FLOAT, 0, normals.data());
    }

    bool isColorMaterialEnabled = false;

    if(plot->hasColors() && !isPicking){
        const SgColorArray& orgColors = *plot->colors();
        ColorArray& colors = buf->colors;
        colors.clear();
        colors.reserve(expandedVertices.size());
        const SgIndexArray& colorIndices = plot->colorIndices();
        if(colorIndices.empty()){
            for(int i=0; i < orgColors.size(); ++i){
                colors.push_back(createColorWithAlpha(orgColors[i]));
            }
        } else {
            for(int i=0; i < colorIndices.size(); ++i){
                colors.push_back(createColorWithAlpha(orgColors[colorIndices[i]]));
            }
        }
        if(plot->hasNormals()){
            glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
            glEnable(GL_COLOR_MATERIAL);
            isColorMaterialEnabled = true;
        }
        glEnableClientState(GL_COLOR_ARRAY);
        glColorPointer(4, GL_FLOAT, 0, &colors[0][0]);
        stateFlag.set(CURRENT_COLOR);
        //setColor(colors.back()); // set the last color
    }
    
    pushPickName(plot);
    glDrawArrays(primitiveMode, 0, expandedVertices.size());
    popPickName();
    
    if(plot->hasNormals()){
        if(isColorMaterialEnabled){
            glDisable(GL_COLOR_MATERIAL);
        }
    } else {
        enableLighting(true);
    }
    
    glPopClientAttrib();
}


void GL1SceneRenderer::visitLineSet(SgLineSet* lineSet)
{
    impl->visitLineSet(lineSet);
}


void GL1SceneRendererImpl::visitLineSet(SgLineSet* lineSet)
{
    const int n = lineSet->numLines();
    if(!lineSet->hasVertices() || (n <= 0)){
        return;
    }

    const SgVertexArray& orgVertices = *lineSet->vertices();
    SgVertexArray& vertices = buf->vertices;
    vertices.clear();
    vertices.reserve(n * 2);
    for(int i=0; i < n; ++i){
        SgLineSet::LineRef line = lineSet->line(i);
        vertices.push_back(orgVertices[line[0]]);
        vertices.push_back(orgVertices[line[1]]);
    }

    const double w = lineSet->lineWidth();
    if(w > 0.0){
        setLineWidth(w);
    }
    renderPlot(lineSet, vertices, GL_LINES);
    if(w > 0.0){
        setLineWidth(defaultLineWidth);
    }
}


void GL1SceneRenderer::visitPreprocessed(SgPreprocessed* preprocessed)
{

}


void GL1SceneRenderer::visitLight(SgLight* light)
{

}


void GL1SceneRenderer::visitOverlay(SgOverlay* overlay)
{
    if(isPicking()){
        return;
    }
    
    glPushAttrib(GL_LIGHTING_BIT);
    glDisable(GL_LIGHTING);
            
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    SgOverlay::ViewVolume v;
    v.left = -1.0;
    v.right = 1.0;
    v.bottom = -1.0;
    v.top = 1.0;
    v.zNear = 1.0;
    v.zFar = -1.0;

    const Array4i vp = viewport();
    overlay->calcViewVolume(vp[2], vp[3], v);

    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(v.left, v.right, v.bottom, v.top, v.zNear, v.zFar);

    visitGroup(overlay);
    
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
    
    glPopAttrib();
}


bool GL1SceneRenderer::isPicking()
{
    return impl->isPicking;
}


void GL1SceneRendererImpl::clearGLState()
{
    stateFlag.reset();
    
    //! \todo get the current state from a GL context
    diffuseColor << 0.0f, 0.0f, 0.0f, 0.0f;
    ambientColor << 0.0f, 0.0f, 0.0f, 0.0f;
    emissionColor << 0.0f, 0.0f, 0.0f, 0.0f;
    specularColor << 0.0f, 0.0f, 0.0f, 0.0f;
    shininess = 0.0f;
    lastAlpha = 1.0f;
    isColorMaterialEnabled = false;
    isCullFaceEnabled = true;
    isCCW = false;
    isLightingEnabled = true;
    isLightModelTwoSide = false;
    isBlendEnabled = true;
    isDepthMaskEnabled = true;

    pointSize = defaultPointSize;
    lineWidth = defaultLineWidth;
}


void GL1SceneRendererImpl::setColor(const Vector4f& color)
{
    if(!isPicking){
        if(!stateFlag[CURRENT_COLOR] || color != currentColor){
            glColor4f(color[0], color[1], color[2], color[3]);
            currentColor = color;
            stateFlag.set(CURRENT_COLOR);
        }
    }
}


void GL1SceneRenderer::setColor(const Vector4f& color)
{
    impl->setColor(color);
}


void GL1SceneRendererImpl::enableColorMaterial(bool on)
{
    if(!isPicking){
        if(!stateFlag[COLOR_MATERIAL] || isColorMaterialEnabled != on){
            if(on){
                glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
                glEnable(GL_COLOR_MATERIAL);
            } else {
                glDisable(GL_COLOR_MATERIAL);
            }
            isColorMaterialEnabled = on;
            stateFlag.set(COLOR_MATERIAL);
        }
    }
}


void GL1SceneRenderer::enableColorMaterial(bool on)
{
    impl->enableColorMaterial(on);
}


void GL1SceneRendererImpl::setDiffuseColor(const Vector4f& color)
{
    if(!stateFlag[DIFFUSE_COLOR] || diffuseColor != color){
        glMaterialfv(GL_FRONT_AND_BACK, GL_DIFFUSE, color.data());
        diffuseColor = color;
        stateFlag.set(DIFFUSE_COLOR);
    }
}


void GL1SceneRenderer::setDiffuseColor(const Vector4f& color)
{
    impl->setDiffuseColor(color);
}


void GL1SceneRendererImpl::setAmbientColor(const Vector4f& color)
{
    if(!stateFlag[AMBIENT_COLOR] || ambientColor != color){
        glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT, color.data());
        ambientColor = color;
        stateFlag.set(AMBIENT_COLOR);
    }
}


void GL1SceneRenderer::setAmbientColor(const Vector4f& color)
{
    impl->setAmbientColor(color);
}


void GL1SceneRendererImpl::setEmissionColor(const Vector4f& color)
{
    if(!stateFlag[EMISSION_COLOR] || emissionColor != color){
        glMaterialfv(GL_FRONT_AND_BACK, GL_EMISSION, color.data());
        emissionColor = color;
        stateFlag.set(EMISSION_COLOR);
    }
}


void GL1SceneRenderer::setEmissionColor(const Vector4f& color)
{
    impl->setEmissionColor(color);
}


void GL1SceneRendererImpl::setSpecularColor(const Vector4f& color)
{
    if(!stateFlag[SPECULAR_COLOR] || specularColor != color){
        glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, color.data());
        specularColor = color;
        stateFlag.set(SPECULAR_COLOR);
    }
}


void GL1SceneRenderer::setSpecularColor(const Vector4f& color)
{
    impl->setSpecularColor(color);
}


void GL1SceneRendererImpl::setShininess(float s)
{
    if(!stateFlag[SHININESS] || shininess != s){
        glMaterialf(GL_FRONT_AND_BACK, GL_SHININESS, s);
        shininess = s;
        stateFlag.set(SHININESS);
    }
}


void GL1SceneRenderer::setShininess(float s)
{
    impl->setShininess(s);
}


void GL1SceneRendererImpl::enableCullFace(bool on)
{
    if(!stateFlag[CULL_FACE] || isCullFaceEnabled != on){
        if(on){
            glEnable(GL_CULL_FACE);
        } else {
            glDisable(GL_CULL_FACE);
        }
        isCullFaceEnabled = on;
        stateFlag.set(CULL_FACE);
    }
}


void GL1SceneRenderer::enableCullFace(bool on)
{
    impl->enableCullFace(on);
}


void GL1SceneRendererImpl::setFrontCCW(bool on)
{
    if(!stateFlag[CCW] || isCCW != on){
        if(on){
            glFrontFace(GL_CCW);
        } else {
            glFrontFace(GL_CW);
        }
        isCCW = on;
        stateFlag.set(CCW);
    }
}


void GL1SceneRenderer::setFrontCCW(bool on)
{
    impl->setFrontCCW(on);
}


/**
   Lighting should not be enabled in rendering code
   which may be rendered with displaylists.
*/
void GL1SceneRendererImpl::enableLighting(bool on)
{
    if(isPicking || !defaultLighting){
        return;
    }
    if(!stateFlag[LIGHTING] || isLightingEnabled != on){
        if(on){
            glEnable(GL_LIGHTING);
        } else {
            glDisable(GL_LIGHTING);
        }
        isLightingEnabled = on;
        stateFlag.set(LIGHTING);
    }
}


void GL1SceneRenderer::enableLighting(bool on)
{
    impl->enableLighting(on);
}


void GL1SceneRendererImpl::setLightModelTwoSide(bool on)
{
    if(!stateFlag[LIGHT_MODEL_TWO_SIDE] || isLightModelTwoSide != on){
        glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, on ? GL_TRUE : GL_FALSE);
        isLightModelTwoSide = on;
        stateFlag.set(LIGHT_MODEL_TWO_SIDE);
    }
}


void GL1SceneRenderer::setLightModelTwoSide(bool on)
{
    impl->setLightModelTwoSide(on);
}


void GL1SceneRenderer::setHeadLightLightingFromBackEnabled(bool on)
{
    impl->isHeadLightLightingFromBackEnabled = on;
}


void GL1SceneRendererImpl::enableBlend(bool on)
{
    if(isPicking){
        return;
    }
    if(!stateFlag[BLEND] || isBlendEnabled != on){
        if(on){
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            enableDepthMask(false);
        } else {
            glDisable(GL_BLEND);
            enableDepthMask(true);
        }
        isBlendEnabled = on;
        stateFlag.set(BLEND);
    }
}


void GL1SceneRenderer::enableBlend(bool on)
{
    impl->enableBlend(on);
}


void GL1SceneRendererImpl::enableDepthMask(bool on)
{
    if(!stateFlag[DEPTH_MASK] || isDepthMaskEnabled != on){
        glDepthMask(on);
        isDepthMaskEnabled = on;
        stateFlag.set(DEPTH_MASK);
    }
}


void GL1SceneRenderer::enableDepthMask(bool on)
{
    impl->enableDepthMask(on);
}


void GL1SceneRendererImpl::setPointSize(float size)
{
    if(!stateFlag[POINT_SIZE] || pointSize != size){
        if(isPicking){
            glPointSize(std::max(size, MinLineWidthForPicking));
        } else {
            glPointSize(size);
        }
        pointSize = size;
        stateFlag.set(POINT_SIZE);
    }
}


void GL1SceneRenderer::setPointSize(float size)
{
    impl->setPointSize(size);
}


void GL1SceneRendererImpl::setLineWidth(float width)
{
    if(!stateFlag[LINE_WIDTH] || lineWidth != width){
        if(isPicking){
            glLineWidth(std::max(width, MinLineWidthForPicking));
        } else {
            glLineWidth(width);
        }
        lineWidth = width;
        stateFlag.set(LINE_WIDTH);
    }
}


void GL1SceneRenderer::setLineWidth(float width)
{
    impl->setLineWidth(width);
}



SgObject* SgCustomGLNode::clone(SgCloneMap& cloneMap) const
{
    return new SgCustomGLNode(*this, cloneMap);
}


void SgCustomGLNode::accept(SceneVisitor& visitor)
{
    GL1SceneRenderer* renderer = dynamic_cast<GL1SceneRenderer*>(&visitor);
    if(renderer){
        renderer->impl->pushPickName(this);
        render(*renderer);
        renderer->impl->popPickName();
    } else {
        visitor.visitGroup(this);
    }
}
    

void SgCustomGLNode::render(GL1SceneRenderer& renderer)
{
    if(renderingFunction){
        renderingFunction(renderer);
    }
}


void SgCustomGLNode::setRenderingFunction(RenderingFunction f)
{
    renderingFunction = f;
}


void GL1SceneRenderer::setDefaultLighting(bool on)
{
    if(on != impl->defaultLighting){
        impl->defaultLighting = on;
        requestToClearCache();
    }
}


void GL1SceneRenderer::setDefaultSmoothShading(bool on)
{
    impl->defaultSmoothShading = on;
}


SgMaterial* GL1SceneRenderer::defaultMaterial()
{
    return impl->defaultMaterial;
}


void GL1SceneRenderer::enableTexture(bool on)
{
    if(on != impl->isTextureEnabled){
        impl->isTextureEnabled = on;
        requestToClearCache();
    }
}


void GL1SceneRenderer::setDefaultPointSize(double size)
{
    if(size != impl->defaultPointSize){
        impl->defaultPointSize = size;
        requestToClearCache();
    }
}


void GL1SceneRenderer::setDefaultLineWidth(double width)
{
    if(width != impl->defaultLineWidth){
        impl->defaultLineWidth = width;
        requestToClearCache();
    }
}


void GL1SceneRenderer::showNormalVectors(double length)
{
    bool doNormalVisualization = (length > 0.0);
    if(doNormalVisualization != impl->doNormalVisualization || length != impl->normalLength){
        impl->doNormalVisualization = doNormalVisualization;
        impl->normalLength = length;
        requestToClearCache();
    }
}


void GL1SceneRenderer::setNewDisplayListDoubleRenderingEnabled(bool on)
{
    impl->isNewDisplayListDoubleRenderingEnabled = on;
}


void GL1SceneRenderer::enableUnusedCacheCheck(bool on)
{
    if(!on){
        impl->nextCacheMap->clear();
    }
    impl->doUnusedCacheCheck = on;
}


const Affine3& GL1SceneRenderer::currentModelTransform() const
{
    impl->currentModelTransform = impl->lastViewMatrix.inverse() * impl->Vstack.back();
    return impl->currentModelTransform;
}


const Matrix4& GL1SceneRenderer::projectionMatrix() const
{
    return impl->lastProjectionMatrix;
}


void GL1SceneRenderer::visitOutlineGroup(SgOutlineGroup* outline)
{
    impl->visitOutlineGroup(outline);
}


void GL1SceneRendererImpl::visitOutlineGroup(SgOutlineGroup* outlineGroup)
{
    glClearStencil(0);
    glClear(GL_STENCIL_BUFFER_BIT);

    glEnable(GL_STENCIL_TEST);

    glStencilFunc(GL_ALWAYS, 1, -1);
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);

    for(SgGroup::const_iterator p = outlineGroup->begin(); p != outlineGroup->end(); ++p){
        (*p)->accept(*self);
    }

    glStencilFunc(GL_NOTEQUAL, 1, -1);
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);

    glPushAttrib(GL_POLYGON_BIT);
    glLineWidth(outlineGroup->lineWidth()*2+1);
    glPolygonMode(GL_FRONT, GL_LINE);
    setColor(outlineGroup->color());
    enableColorMaterial(true);
    for(SgGroup::const_iterator p = outlineGroup->begin(); p != outlineGroup->end(); ++p){
        (*p)->accept(*self);
    }
    enableColorMaterial(false);
    setLineWidth(lineWidth);
    glPopAttrib();

    glDisable(GL_STENCIL_TEST);
}
