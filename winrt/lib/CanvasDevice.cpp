// Copyright (c) Microsoft Corporation. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use these files except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
// WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
// License for the specific language governing permissions and limitations
// under the License.

#include "pch.h"
#include "CanvasDevice.h"
#include "CanvasImage.h"

namespace ABI { namespace Microsoft { namespace Graphics { namespace Canvas
{
    //
    // This implementation of the adapter does the normal thing, and calls into
    // D2D. It is separated out so tests can inject their own implementation.
    //
    ComPtr<ID2D1Factory2> DefaultDeviceResourceCreationAdapter::CreateD2DFactory(
        CanvasDebugLevel debugLevel)
    {
        D2D1_FACTORY_OPTIONS factoryOptions;
        factoryOptions.debugLevel = static_cast<D2D1_DEBUG_LEVEL>(debugLevel);

        ComPtr<ID2D1Factory2> createdFactory;
        ThrowIfFailed(D2D1CreateFactory(
            D2D1_FACTORY_TYPE_MULTI_THREADED, 
            __uuidof(ID2D1Factory2), 
            &factoryOptions, 
            &createdFactory));

        return createdFactory;
    }

    bool DefaultDeviceResourceCreationAdapter::TryCreateD3DDevice(
        CanvasHardwareAcceleration hardwareAcceleration, 
        ComPtr<ID3D11Device>* device)
    {
        assert(hardwareAcceleration == CanvasHardwareAcceleration::On || hardwareAcceleration == CanvasHardwareAcceleration::Off);

        D3D_DRIVER_TYPE driverType = 
            hardwareAcceleration == CanvasHardwareAcceleration::On ? D3D_DRIVER_TYPE_HARDWARE : D3D_DRIVER_TYPE_WARP;

        ComPtr<ID3D11Device> createdDevice;
        if (SUCCEEDED(D3D11CreateDevice(
            NULL, // adapter
            driverType,
            NULL, // software handle
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            NULL, // feature level array
            0,  // feature level count
            D3D11_SDK_VERSION,
            &createdDevice,
            NULL,
            NULL)))
        {
            *device = createdDevice;
            return true;
        }
        else
        {
            return false;
        }
    }

    //
    // We want to find the DXGI device associated with an ID2D1Device.
    // Unfortunately, there's no direct way to do this.  However, it is possible
    // to figure it out by creating a bitmap and querying that for the dxgi
    // surface and then its dxgi device.
    //
    // #1428 tracks getting direct support for this added to D2D.
    //
    ComPtr<IDXGIDevice3> DefaultDeviceResourceCreationAdapter::GetDxgiDevice(ID2D1Device1* d2dDevice)
    {
        ComPtr<ID2D1DeviceContext> deviceContext;
        ThrowIfFailed(d2dDevice->CreateDeviceContext(
            D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
            &deviceContext));

        ComPtr<ID2D1Bitmap1> bitmap;
        ThrowIfFailed(deviceContext->CreateBitmap(
            D2D1_SIZE_U{ 1, 1 },
            nullptr,
            0,
            D2D1::BitmapProperties1(
                D2D1_BITMAP_OPTIONS_NONE,
                D2D1::PixelFormat(DXGI_FORMAT_R8G8B8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)),
            &bitmap));

        ComPtr<IDXGISurface> surface;
        ThrowIfFailed(bitmap->GetSurface(&surface));

        ComPtr<IDXGIDevice3> device;
        ThrowIfFailed(surface->GetDevice(IID_PPV_ARGS(&device)));

        return device;
    }

    //
    // CanvasDeviceManager
    //

    void ThrowIfInvalid(CanvasDebugLevel debugLevel)
    {
        switch (debugLevel)
        {
        case CanvasDebugLevel::None:
        case CanvasDebugLevel::Warning:
        case CanvasDebugLevel::Error:
        case CanvasDebugLevel::Information:
            break;
        default:
            ThrowHR(E_INVALIDARG);
        }
    }

    CanvasDeviceManager::CanvasDeviceManager(
        std::shared_ptr<ICanvasDeviceResourceCreationAdapter> adapter)
        : m_adapter(adapter)
    {
    }


    ComPtr<CanvasDevice> CanvasDeviceManager::CreateNew(
        CanvasDebugLevel debugLevel, 
        CanvasHardwareAcceleration requestedHardwareAcceleration)
    {
        ThrowIfInvalid(debugLevel);

        auto d2dFactory = m_adapter->CreateD2DFactory(debugLevel);

        return CreateNew(debugLevel, requestedHardwareAcceleration, d2dFactory.Get());
    }


    ComPtr<CanvasDevice> CanvasDeviceManager::CreateNew(
        CanvasDebugLevel debugLevel,
        CanvasHardwareAcceleration requestedHardwareAcceleration,
        ID2D1Factory2* d2dFactory)
    {
        ThrowIfInvalid(debugLevel);
        CheckInPointer(d2dFactory);

        CanvasHardwareAcceleration hardwareAcceleration;

        auto dxgiDevice = MakeDXGIDevice(
            requestedHardwareAcceleration,
            &hardwareAcceleration);

        ComPtr<ID2D1Device1> d2dDevice;
        ThrowIfFailed(d2dFactory->CreateDevice(dxgiDevice.Get(), &d2dDevice));
        
        auto device = Make<CanvasDevice>(
            shared_from_this(), 
            debugLevel,
            hardwareAcceleration,
            dxgiDevice.Get(),
            d2dDevice.Get());
        CheckMakeResult(device);

        return device;
    }

    
    ComPtr<CanvasDevice> CanvasDeviceManager::CreateNew(
        CanvasDebugLevel debugLevel,
        IDirect3DDevice* direct3DDevice)
    {
        ThrowIfInvalid(debugLevel);
        CheckInPointer(direct3DDevice);
        auto d2dFactory = m_adapter->CreateD2DFactory(debugLevel);

        return CreateNew(debugLevel, direct3DDevice, d2dFactory.Get());
    }


    ComPtr<CanvasDevice> CanvasDeviceManager::CreateNew(
        CanvasDebugLevel debugLevel,
        IDirect3DDevice* direct3DDevice,
        ID2D1Factory2* d2dFactory)
    {
        ThrowIfInvalid(debugLevel);
        CheckInPointer(direct3DDevice);
        CheckInPointer(d2dFactory);

        ComPtr<IDXGIInterfaceAccess> dxgiInterfaceAccess;
        ThrowIfFailed(direct3DDevice->QueryInterface(IID_PPV_ARGS(&dxgiInterfaceAccess)));

        ComPtr<IDXGIDevice3> dxgiDevice;
        ThrowIfFailed(dxgiInterfaceAccess->GetDXGIInterface(IID_PPV_ARGS(&dxgiDevice)));

        ComPtr<ID2D1Device1> d2dDevice;
        ThrowIfFailed(d2dFactory->CreateDevice(dxgiDevice.Get(), &d2dDevice));
        
        auto device = Make<CanvasDevice>(
            shared_from_this(), 
            debugLevel, 
            CanvasHardwareAcceleration::Unknown,
            dxgiDevice.Get(), 
            d2dDevice.Get());
        CheckMakeResult(device);

        return device;
    }

    
    ComPtr<CanvasDevice> CanvasDeviceManager::CreateWrapper(
        ID2D1Device1* d2dDevice)
    {
        auto dxgiDevice = m_adapter->GetDxgiDevice(d2dDevice);

        auto canvasDevice = Make<CanvasDevice>(
            shared_from_this(),
            CanvasDebugLevel::None,
            CanvasHardwareAcceleration::Unknown,
            dxgiDevice.Get(),
            d2dDevice);
        CheckMakeResult(canvasDevice);
        
        return canvasDevice;
    }


    ComPtr<IDXGIDevice3> CanvasDeviceManager::MakeDXGIDevice(
        CanvasHardwareAcceleration requestedHardwareAcceleration,
        CanvasHardwareAcceleration* actualHardwareAccelerationOut) const
    {
        assert(actualHardwareAccelerationOut);
        *actualHardwareAccelerationOut = CanvasHardwareAcceleration::Unknown;

        CanvasHardwareAcceleration actualHardwareAcceleration = CanvasHardwareAcceleration::Unknown;

        ComPtr<ID3D11Device> d3dDevice = MakeD3D11Device(
            requestedHardwareAcceleration,
            &actualHardwareAcceleration);

        ComPtr<IDXGIDevice3> dxgiDevice;
        ThrowIfFailed(d3dDevice.As(&dxgiDevice));

        *actualHardwareAccelerationOut = actualHardwareAcceleration;

        return dxgiDevice;
    }


    ComPtr<ID3D11Device> CanvasDeviceManager::MakeD3D11Device(
        CanvasHardwareAcceleration requestedHardwareAcceleration,
        CanvasHardwareAcceleration* actualHardwareAccelerationOut) const
    {
        assert(actualHardwareAccelerationOut);

        ComPtr<ID3D11Device> d3dDevice;

        switch (requestedHardwareAcceleration)
        {
        case CanvasHardwareAcceleration::Auto:
            if (m_adapter->TryCreateD3DDevice(CanvasHardwareAcceleration::On, &d3dDevice))
            {
                *actualHardwareAccelerationOut = CanvasHardwareAcceleration::On;
                return d3dDevice;
            }

            if (m_adapter->TryCreateD3DDevice(CanvasHardwareAcceleration::Off, &d3dDevice))
            {
                *actualHardwareAccelerationOut = CanvasHardwareAcceleration::Off;
                return d3dDevice;
            }

            break;

        case CanvasHardwareAcceleration::On:
        case CanvasHardwareAcceleration::Off:
            if (m_adapter->TryCreateD3DDevice(requestedHardwareAcceleration, &d3dDevice))
            {
                *actualHardwareAccelerationOut = requestedHardwareAcceleration;
                return d3dDevice;
            }

            break;

        default:
            ThrowHR(E_INVALIDARG);
        }

        // If we end up here then we failed to create a d3d device
        ThrowHR(E_FAIL);
    }


    //
    // CanvasDeviceFactory
    //

    std::shared_ptr<CanvasDeviceManager> CanvasDeviceFactory::CreateManager()
    {
        auto adapter = std::make_shared<DefaultDeviceResourceCreationAdapter>();
        return std::make_shared<CanvasDeviceManager>(adapter);
    }

    IFACEMETHODIMP CanvasDeviceFactory::CreateWithDebugLevel(
        CanvasDebugLevel debugLevel,
        ICanvasDevice** canvasDevice)
    {
        return CreateWithDebugLevelAndHardwareAcceleration(
            debugLevel,
            CanvasHardwareAcceleration::Auto,
            canvasDevice);
    }

    IFACEMETHODIMP CanvasDeviceFactory::CreateWithDebugLevelAndHardwareAcceleration(
        CanvasDebugLevel debugLevel,
        CanvasHardwareAcceleration hardwareAcceleration,
        ICanvasDevice** canvasDevice)
    {
        return ExceptionBoundary(
            [&]
            {
                CheckAndClearOutPointer(canvasDevice);
                
                auto newCanvasDevice = GetManager()->Create(debugLevel, hardwareAcceleration);
                
                ThrowIfFailed(newCanvasDevice.CopyTo(canvasDevice));
            });
    }

    IFACEMETHODIMP CanvasDeviceFactory::CreateFromDirect3D11Device(
        CanvasDebugLevel debugLevel,
        IDirect3DDevice* direct3DDevice,
        ICanvasDevice** canvasDevice)
    {
        return ExceptionBoundary(
            [&]
            {
                CheckInPointer(direct3DDevice);
                CheckAndClearOutPointer(canvasDevice);

                auto newCanvasDevice = GetManager()->Create(debugLevel, direct3DDevice);

                ThrowIfFailed(newCanvasDevice.CopyTo(canvasDevice));
            });
    }

    IFACEMETHODIMP CanvasDeviceFactory::GetOrCreate(
        IUnknown* resource,
        IInspectable** wrapper)
    {
        return ExceptionBoundary(
            [&]
            {
                CheckInPointer(resource);
                CheckAndClearOutPointer(wrapper);

                ComPtr<ID2D1Device1> d2dDevice;
                ThrowIfFailed(resource->QueryInterface(d2dDevice.GetAddressOf()));

                auto newCanvasDevice = GetManager()->GetOrCreate(d2dDevice.Get());

                ThrowIfFailed(newCanvasDevice.CopyTo(wrapper));
            });
    }

    _Use_decl_annotations_
    IFACEMETHODIMP CanvasDeviceFactory::ActivateInstance(IInspectable **object)
    {
        return ExceptionBoundary(
            [&]
            {
                CheckAndClearOutPointer(object);

                auto newCanvasDevice = GetManager()->Create(
                    CanvasDebugLevel::None,
                    CanvasHardwareAcceleration::Auto);

                ThrowIfFailed(newCanvasDevice.CopyTo(object));
            });
    }

    //
    // CanvasDevice
    //

    CanvasDevice::CanvasDevice(
        std::shared_ptr<CanvasDeviceManager> deviceManager,
        CanvasDebugLevel debugLevel,
        CanvasHardwareAcceleration hardwareAcceleration,
        IDXGIDevice3* dxgiDevice,
        ID2D1Device1* d2dDevice)
        : ResourceWrapper(deviceManager, d2dDevice)
        , m_hardwareAcceleration(hardwareAcceleration)
        , m_debugLevel(debugLevel)
        , m_dxgiDevice(dxgiDevice)
    {
        CheckInPointer(dxgiDevice);

        ThrowIfFailed(d2dDevice->CreateDeviceContext(
            D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
            &m_d2dResourceCreationDeviceContext));
    }

    ComPtr<ID2D1Factory2> CanvasDevice::GetD2DFactory()
    {
        ComPtr<ID2D1Device1> d2dDevice = GetD2DDevice();

        ComPtr<ID2D1Factory> d2dBaseFactory;
        d2dDevice->GetFactory(&d2dBaseFactory);

        ComPtr<ID2D1Factory2> d2dDerivedFactory;
        d2dBaseFactory.As(&d2dDerivedFactory);

        return d2dDerivedFactory;
    }

    IFACEMETHODIMP CanvasDevice::get_HardwareAcceleration(_Out_ CanvasHardwareAcceleration *value)
    {
        return ExceptionBoundary(
            [&]
            {
                CheckInPointer(value);
                GetResource();  // this ensures that Close() hasn't been called
                *value = m_hardwareAcceleration;
                return S_OK;
            });
    }

    IFACEMETHODIMP CanvasDevice::Close()
    {
        HRESULT hr = ResourceWrapper::Close();
        if (FAILED(hr))
            return hr;
        
        m_dxgiDevice.Close();
        m_d2dResourceCreationDeviceContext.Close();
        return S_OK;
    }

    ComPtr<ID2D1Device1> CanvasDevice::GetD2DDevice()
    {
        return GetResource();
    }

    ComPtr<ID2D1SolidColorBrush> CanvasDevice::CreateSolidColorBrush(D2D1_COLOR_F const& color)
    {
        // TODO #802: this isn't very threadsafe - we should really have a different
        // resource creation context per-thread.
        auto deviceContext = m_d2dResourceCreationDeviceContext.EnsureNotClosed();

        ComPtr<ID2D1SolidColorBrush> brush;
        ThrowIfFailed(deviceContext->CreateSolidColorBrush(color, &brush));

        return brush;
    }

    ComPtr<ID2D1Bitmap1> CanvasDevice::CreateBitmap(IWICFormatConverter* wicConverter)
    {
        auto deviceContext = m_d2dResourceCreationDeviceContext.EnsureNotClosed();

        ComPtr<ID2D1Bitmap1> bitmap;
        ThrowIfFailed(deviceContext->CreateBitmapFromWicBitmap(wicConverter, &bitmap));

        return bitmap;
    }
    
    IFACEMETHODIMP CanvasDevice::get_Device(ICanvasDevice** value)
    {
        return ExceptionBoundary(
            [&]
            {
                CheckAndClearOutPointer(value);
                ComPtr<ICanvasDevice> device(this);
                *value = device.Detach();
            });
    }

    ComPtr<ID2D1BitmapBrush1> CanvasDevice::CreateBitmapBrush(ID2D1Bitmap1* bitmap)
    {
        auto deviceContext = m_d2dResourceCreationDeviceContext.EnsureNotClosed();

        ComPtr<ID2D1BitmapBrush1> bitmapBrush;
        ThrowIfFailed(deviceContext->CreateBitmapBrush(bitmap, &bitmapBrush));

        return bitmapBrush;
    }

    ComPtr<ID2D1ImageBrush> CanvasDevice::CreateImageBrush(ID2D1Image* image)
    {
        auto deviceContext = m_d2dResourceCreationDeviceContext.EnsureNotClosed();

        ComPtr<ID2D1ImageBrush> imageBrush;
        ThrowIfFailed(deviceContext->CreateImageBrush(image, D2D1::ImageBrushProperties(D2D1::RectF()), &imageBrush));

        return imageBrush;
    }

    ComPtr<ID2D1Image> CanvasDevice::GetD2DImage(ICanvasImage* canvasImage)
    {
        ComPtr<ICanvasImageInternal> imageInternal;
        ThrowIfFailed(canvasImage->QueryInterface(imageInternal.GetAddressOf()));

        auto deviceContext = m_d2dResourceCreationDeviceContext.EnsureNotClosed();
        return imageInternal->GetD2DImage(deviceContext.Get());
    }

    IFACEMETHODIMP CanvasDevice::Trim()
    {
        return ExceptionBoundary(
            [&]
            {
                auto& dxgiDevice = m_dxgiDevice.EnsureNotClosed();

                dxgiDevice->Trim();
            });
    }

    IFACEMETHODIMP CanvasDevice::GetDXGIInterface(REFIID iid, void** p)
    {
        return ExceptionBoundary(
            [&]
            {
                auto& dxgiDevice = m_dxgiDevice.EnsureNotClosed();

                ThrowIfFailed(dxgiDevice.CopyTo(iid, p));
            });
    }

    ActivatableClassWithFactory(CanvasDevice, CanvasDeviceFactory);
}}}}
