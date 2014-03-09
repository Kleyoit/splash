#include "image.h"
#include "timer.h"

#include <OpenImageIO/imageio.h>
#include <OpenImageIO/imagebufalgo.h>

using namespace std;

namespace Splash {

/*************/
Image::Image()
{
    _type = "image";

    oiio::attribute("threads", 0); // Disable the thread limitation for OIIO
    createDefaultImage();

    registerAttributes();
}

/*************/
Image::Image(oiio::ImageSpec spec)
{
    _type = "image";
    oiio::attribute("threads", 0);
    set(spec.width, spec.height, spec.nchannels, spec.format);

    registerAttributes();
}

/*************/
Image::~Image()
{
#ifdef DEBUG
    SLog::log << Log::DEBUGGING << "Image::~Image - Destructor" << Log::endl;
#endif
}

/*************/
const void* Image::data() const
{
    return _image.localpixels();
}

/*************/
oiio::ImageBuf Image::get() const
{
    oiio::ImageBuf img;
    lock_guard<mutex> lock(_mutex);
    img.copy(_image);
    return img;
}

/*************/
oiio::ImageSpec Image::getSpec() const
{
    lock_guard<mutex> lock(_mutex);
    return _image.spec();
}

/*************/
void Image::set(const oiio::ImageBuf& img)
{
    lock_guard<mutex> lock(_mutex);
    _image.copy(img);
}

/*************/
void Image::set(unsigned int w, unsigned int h, unsigned int channels, oiio::TypeDesc type)
{
    oiio::ImageSpec spec(w, h, channels, type);
    oiio::ImageBuf img(spec);

    lock_guard<mutex> lock(_mutex);
    _image.swap(img);
    updateTimestamp();
}

/*************/
SerializedObjectPtr Image::serialize() const
{
    _mutex.lock();

    STimer::timer << "serialize " + _name;

    // We first get the xml version of the specs, and pack them into the obj
    string xmlSpec = _image.spec().to_xml();
    int nbrChar = xmlSpec.size();
    int imgSize = _image.spec().pixel_bytes() * _image.spec().width * _image.spec().height;
    int totalSize = sizeof(nbrChar) + nbrChar + imgSize;
    
    if (_serializedBuffers[0].get() == nullptr || _serializedBuffers[1].get() == nullptr
        || _serializedBuffers[0]->size() != totalSize || _serializedBuffers[1]->size() != totalSize)
    {
        _serializedBuffers[0].reset(new SerializedObject(totalSize));
        _serializedBuffers[1].reset(new SerializedObject(totalSize));
    }
    
    SerializedObjectPtr obj = _serializedBuffers[_serializedBufferIndex];
    _serializedBufferIndex = (_serializedBufferIndex + 1) % 2;

    auto currentObjPtr = obj->data();
    const unsigned char* ptr = reinterpret_cast<const unsigned char*>(&nbrChar);
    copy(ptr, ptr + sizeof(nbrChar), currentObjPtr);
    currentObjPtr += sizeof(nbrChar);

    const unsigned char* charPtr = reinterpret_cast<const unsigned char*>(xmlSpec.c_str());
    copy(charPtr, charPtr + nbrChar, currentObjPtr);
    currentObjPtr += nbrChar;

    // And then, the image
    const unsigned char* imgPtr = reinterpret_cast<const unsigned char*>(_image.localpixels());
    if (imgPtr == NULL)
        return SerializedObjectPtr();
    copy(imgPtr, imgPtr + imgSize, currentObjPtr);

    STimer::timer >> "serialize " + _name;

    _mutex.unlock();
    return obj;
}

/*************/
bool Image::deserialize(const SerializedObjectPtr obj)
{
    if (obj->size() == 0)
        return false;

    STimer::timer << "deserialize " + _name;

    // First, we get the size of the metadata
    int nbrChar;
    unsigned char* ptr = reinterpret_cast<unsigned char*>(&nbrChar);

    auto currentObjPtr = obj->data();
    copy(currentObjPtr, currentObjPtr + sizeof(nbrChar), ptr);
    currentObjPtr += sizeof(nbrChar);

    try
    {
        char xmlSpecChar[nbrChar];
        ptr = reinterpret_cast<unsigned char*>(xmlSpecChar);
        copy(currentObjPtr, currentObjPtr + nbrChar, ptr);
        currentObjPtr += nbrChar;
        string xmlSpec(xmlSpecChar);

        oiio::ImageSpec spec;
        spec.from_xml(xmlSpec.c_str());

        oiio::ImageBuf image(spec);
        int imgSize = image.spec().pixel_bytes() * image.spec().width * image.spec().height;
        ptr = reinterpret_cast<unsigned char*>(image.localpixels());
        copy(currentObjPtr, currentObjPtr + imgSize, ptr);

        bool isLocked {false};
        if (obj != _serializedObject) // If we are not setting the image from the inner serialized buffer
        {
            isLocked = true;
            _mutex.lock();
        }
        _bufferImage.swap(image);
        _imageUpdated = true;

        updateTimestamp();

        if (isLocked)
            _mutex.unlock();
    }
    catch (...)
    {
        SLog::log << Log::ERROR << "Image::" << __FUNCTION__ << " - Unable to deserialize the given object" << Log::endl;
        return false;
    }

    STimer::timer >> "deserialize " + _name;

    return true;
}

/*************/
bool Image::read(const string& filename)
{
    oiio::ImageInput* in = oiio::ImageInput::open(filename);
    if (!in)
    {
        SLog::log << Log::WARNING << "Image::" << __FUNCTION__ << " - Unable to load file " << filename << Log::endl;
        return false;
    }

    const oiio::ImageSpec& spec = in->spec();
    if (spec.format != oiio::TypeDesc::UINT8)
    {
        SLog::log << Log::WARNING << "Image::" << __FUNCTION__ << " - Only 8bit images are supported." << Log::endl;
        return false;
    }

    int xres = spec.width;
    int yres = spec.height;
    int channels = spec.nchannels;
    oiio::ImageBuf img(spec); 
    in->read_image(oiio::TypeDesc::UINT8, img.localpixels());

    in->close();
    delete in;

    // If the image has only 3 channels, we add one
    if (channels == 3)
    {
        oiio::ImageSpec newSpec(xres, yres, 4, oiio::TypeDesc::UINT8);
        oiio::ImageBuf newImg(newSpec);
        char* inputPixels = (char*)img.localpixels();
        char* newPixels = (char*)newImg.localpixels();

        if (inputPixels == nullptr || newPixels == nullptr)
            return false;

        for (int y = 0; y < yres; ++y)
            for (int x = 0; x < xres; ++x)
            {
                memcpy(&newPixels[(x + y * xres) * 4], &inputPixels[(x + y * xres) * 3], 3 * sizeof(char));
                newPixels[(x + y * xres) * 4 + 3] = 255;
            }

        channels = 4;
        img.swap(newImg);
    }
    else if (channels != 4)
    {
        return false;
    }

    lock_guard<mutex> lock(_mutex);
    _bufferImage.swap(img);
    _imageUpdated = true;

    updateTimestamp();

    return true;
}

/*************/
void Image::setTo(float value)
{
    lock_guard<mutex> lock(_mutex);
    float v[_image.nchannels()];
    for (int i = 0; i < _image.nchannels(); ++i)
        v[i] = (float)value;
    oiio::ImageBufAlgo::fill(_image, v);
}

/*************/
void Image::update()
{
    lock_guard<mutex> lock(_mutex);
    if (_imageUpdated)
    {
        _image.swap(_bufferImage);
        _imageUpdated = false;
    }
    else if (_benchmark)
        updateTimestamp();
}

/*************/
void Image::createDefaultImage()
{
    oiio::ImageSpec spec(512, 512, 4, oiio::TypeDesc::UINT8);
    oiio::ImageBuf img(spec);

    for (oiio::ImageBuf::Iterator<unsigned char> p(img); !p.done(); ++p)
    {
        if (!p.exists())
            continue;

        if (p.x() % 16 > 7 && p.y() % 64 > 31)
            for (int c = 0; c < img.nchannels(); ++c)
                p[c] = 255;
        else
            for (int c = 0; c < img.nchannels(); ++c)
                p[c] = 0;
    }

    lock_guard<mutex> lock(_mutex);
    _image.swap(img);
    updateTimestamp();
}

/*************/
void Image::registerAttributes()
{
    _attribFunctions["file"] = AttributeFunctor([&](vector<Value> args) {
        if (args.size() < 1)
            return false;
        return read(args[0].asString());
    });

    _attribFunctions["srgb"] = AttributeFunctor([&](vector<Value> args) {
        if (args.size() < 1)
            return false;
        _srgb = (args[0].asInt() > 0) ? true : false;     
        return true;
    }, [&]() {
        return vector<Value>({_srgb});
    });

    _attribFunctions["benchmark"] = AttributeFunctor([&](vector<Value> args) {
        if (args.size() < 1)
            return false;
        if (args[0].asInt() > 0)
            _benchmark = true;
        else
            _benchmark = false;
        return true;
    });
}

} // end of namespace
