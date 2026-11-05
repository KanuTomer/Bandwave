#pragma once
#include <d3d11.h>
#include <d3dcompiler.h>
#include <d2d1_1.h>
#include <winrt/base.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <vector>
#include <dxgi1_2.h>
#include <filesystem>
#include <fstream>
#include "wave_frame.hpp"

// The line shader, shared by the main stage (stage_chain.hpp) and the EQ editor (editor_chain.hpp).
struct LinePipeline {
    // ---- The line shader (0.5.17) ----
    // Direct2D's antialiased strokes of long curved lines were most of the stage's GPU time: about
    // half for the crisp lines and a quarter for the glow strokes at 2560x1600 (measured with parts
    // switched off, 4 Nov 2026). Each line is now a strip of quads, two vertices per point, offset
    // along the line's normal, all drawn by the GPU in one call. The pixel shader works out each
    // pixel's coverage from its distance across the line, so edges stay smooth. Neighbouring quads
    // share their edge exactly, so a translucent line never doubles up at its joins.
    struct LineVertex{float x,y,across,half,soft,r,g,b,a;};
    winrt::com_ptr<ID3D11Device> d3d;winrt::com_ptr<ID3D11DeviceContext> d3dc;
    winrt::com_ptr<ID3D11VertexShader> line_vs;winrt::com_ptr<ID3D11PixelShader> line_ps;winrt::com_ptr<ID3D11InputLayout> line_layout;
    winrt::com_ptr<ID3D11Buffer> line_buffer,line_constants;winrt::com_ptr<ID3D11BlendState> line_blend;winrt::com_ptr<ID3D11RasterizerState> line_raster;
    // Dithering (0.5.19): dark gradients in 8 bits show as bands on the panel even though screenshots
    // look smooth. A faint noise of about one step, added in one pass and taken away in another, breaks
    // the bands up without brightening anything on average.
    winrt::com_ptr<ID3D11VertexShader> dither_vs;winrt::com_ptr<ID3D11PixelShader> dither_ps;winrt::com_ptr<ID3D11BlendState> dither_add,dither_take;float dither_seed=0;
    UINT line_capacity=0;std::vector<LineVertex> line_vertices;
    void create(ID3D11Device* device){
        d3d=nullptr;d3d.copy_from(device);
        static constexpr char source[]=
            "cbuffer Target:register(b0){float2 size;float fade;float unused;float left;float right;float side;float pad;};\n"
            "struct In{float2 p:POSITION;float across:ACROSS;float half:HALF;float soft:SOFT;float4 c:COLOR;};\n"
            "struct Out{float4 p:SV_Position;float across:ACROSS;float half:HALF;float soft:SOFT;float4 c:COLOR;};\n"
            "Out vs(In v){Out o;o.p=float4(v.p.x/size.x*2-1,1-v.p.y/size.y*2,0,1);o.across=v.across;o.half=v.half;o.soft=v.soft;o.c=v.c;return o;}\n"
            // Coverage ramps over the soft width around the edge: one pixel for a crisp line, wider for
            // an out-of-focus one. Near the top and bottom of the target everything fades out.
            "float4 ps(Out i):SV_Target{float cover=saturate((i.half+i.soft*.5-abs(i.across))/i.soft);"
            "float bottom=unused>0?unused:size.y;float edge=fade>0?saturate(i.p.y/fade)*saturate((bottom-i.p.y)/fade):1;float inside=side>0?saturate((i.p.x-left)/side)*saturate((right-i.p.x)/side):1;return i.c*cover*edge*inside;}\n"
            "float4 full(uint id:SV_VertexID):SV_Position{float2 uv=float2((id<<1)&2,id&2);return float4(uv*float2(2,-2)+float2(-1,1),0,1);}\n"
            "float noise(float2 p){p=frac(p*float2(443.897,441.423));p+=dot(p,p.yx+19.19);return frac((p.x+p.y)*p.x);}\n"
            "float4 grain(float4 p:SV_Position):SV_Target{float n=noise(p.xy+unused)*(1.5/255);return float4(n,n,n,0);}\n";
        const auto compile=[&](const char* entry,const char* profile){winrt::com_ptr<ID3DBlob> code,errors;
            if(FAILED(D3DCompile(source,sizeof(source)-1,"stage_lines",nullptr,nullptr,entry,profile,D3DCOMPILE_OPTIMIZATION_LEVEL3,0,code.put(),errors.put())))
                throw std::runtime_error(errors?static_cast<const char*>(errors->GetBufferPointer()):"Line shader did not compile.");
            return code;};
        auto vcode=compile("vs","vs_4_0"),pcode=compile("ps","ps_4_0"),fcode=compile("full","vs_4_0"),gcode=compile("grain","ps_4_0");
        winrt::check_hresult(d3d->CreateVertexShader(fcode->GetBufferPointer(),fcode->GetBufferSize(),nullptr,dither_vs.put()));
        winrt::check_hresult(d3d->CreatePixelShader(gcode->GetBufferPointer(),gcode->GetBufferSize(),nullptr,dither_ps.put()));
        {D3D11_BLEND_DESC add{};auto& a=add.RenderTarget[0];a.BlendEnable=TRUE;a.SrcBlend=a.DestBlend=a.SrcBlendAlpha=a.DestBlendAlpha=D3D11_BLEND_ONE;a.BlendOp=a.BlendOpAlpha=D3D11_BLEND_OP_ADD;a.RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_RED|D3D11_COLOR_WRITE_ENABLE_GREEN|D3D11_COLOR_WRITE_ENABLE_BLUE;
         winrt::check_hresult(d3d->CreateBlendState(&add,dither_add.put()));a.BlendOp=a.BlendOpAlpha=D3D11_BLEND_OP_REV_SUBTRACT;winrt::check_hresult(d3d->CreateBlendState(&add,dither_take.put()));}
        winrt::check_hresult(d3d->CreateVertexShader(vcode->GetBufferPointer(),vcode->GetBufferSize(),nullptr,line_vs.put()));
        winrt::check_hresult(d3d->CreatePixelShader(pcode->GetBufferPointer(),pcode->GetBufferSize(),nullptr,line_ps.put()));
        const D3D11_INPUT_ELEMENT_DESC layout[]{{"POSITION",0,DXGI_FORMAT_R32G32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},{"ACROSS",0,DXGI_FORMAT_R32_FLOAT,0,8,D3D11_INPUT_PER_VERTEX_DATA,0},
            {"HALF",0,DXGI_FORMAT_R32_FLOAT,0,12,D3D11_INPUT_PER_VERTEX_DATA,0},{"SOFT",0,DXGI_FORMAT_R32_FLOAT,0,16,D3D11_INPUT_PER_VERTEX_DATA,0},
            {"COLOR",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,20,D3D11_INPUT_PER_VERTEX_DATA,0}};
        winrt::check_hresult(d3d->CreateInputLayout(layout,5,vcode->GetBufferPointer(),vcode->GetBufferSize(),line_layout.put()));
        D3D11_BUFFER_DESC constants{32,D3D11_USAGE_DYNAMIC,D3D11_BIND_CONSTANT_BUFFER,D3D11_CPU_ACCESS_WRITE,0,0};winrt::check_hresult(d3d->CreateBuffer(&constants,nullptr,line_constants.put()));
        // Premultiplied source-over, the same blending the Polylines and Direct2D strokes had.
        D3D11_BLEND_DESC blend{};auto& rt=blend.RenderTarget[0];rt.BlendEnable=TRUE;rt.SrcBlend=rt.SrcBlendAlpha=D3D11_BLEND_ONE;rt.DestBlend=rt.DestBlendAlpha=D3D11_BLEND_INV_SRC_ALPHA;
        rt.BlendOp=rt.BlendOpAlpha=D3D11_BLEND_OP_ADD;rt.RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;winrt::check_hresult(d3d->CreateBlendState(&blend,line_blend.put()));
        D3D11_RASTERIZER_DESC raster{};raster.FillMode=D3D11_FILL_SOLID;raster.CullMode=D3D11_CULL_NONE;raster.DepthClipEnable=TRUE;winrt::check_hresult(d3d->CreateRasterizerState(&raster,line_raster.put()));
        d3dc=nullptr;d3d->GetImmediateContext(d3dc.put());line_capacity=0;line_buffer=nullptr;
    }
    // One line as a strip of quads: points in DIPs, moved by (dx,dy), scaled to pixels.
    // soft: the width in pixels over which the edge fades (1 is a crisp antialiased edge).
    void add_line(std::vector<dusk::WavePoint> const& points,float dx,float dy,float scale,float half,float r,float g,float b,float a,float soft=1){
        if(points.size()<2||a<=0)return;
        soft=std::max(1.f,soft);const float reach=half+soft; // past the edge far enough for the soft fade
        const size_t n=points.size();std::vector<std::array<float,4>> side(n);
        for(size_t k=0;k<n;++k){
            const auto& before=points[k?k-1:k];const auto& after=points[k+1<n?k+1:k];
            float tx=after.x-before.x,ty=after.y-before.y;const float length=std::sqrt(tx*tx+ty*ty);if(length>0){tx/=length;ty/=length;}else{tx=1;ty=0;}
            const float x=(points[k].x+dx)*scale,y=(points[k].y+dy)*scale,nx=-ty*reach,ny=tx*reach;
            side[k]={x-nx,y-ny,x+nx,y+ny};
        }
        for(size_t k=0;k+1<n;++k){
            const LineVertex l0{side[k][0],side[k][1],-reach,half,soft,r,g,b,a},r0{side[k][2],side[k][3],reach,half,soft,r,g,b,a},
                             l1{side[k+1][0],side[k+1][1],-reach,half,soft,r,g,b,a},r1{side[k+1][2],side[k+1][3],reach,half,soft,r,g,b,a};
            line_vertices.insert(line_vertices.end(),{l0,r0,l1,r0,r1,l1});
        }
    }
    // fade: lines fade out over this many pixels at the top and at `bottom` (the target's bottom if 0).
    // side: everything also fades out over this many pixels inside `left` and `right` (off if 0).
    void flush(ID3D11RenderTargetView* view,float width,float height,float fade=0,float bottom=0,float left=0,float right=0,float side=0){
        if(line_vertices.empty())return;
        const UINT count=static_cast<UINT>(line_vertices.size());
        if(count>line_capacity){line_capacity=count+count/2;line_buffer=nullptr;D3D11_BUFFER_DESC desc{static_cast<UINT>(line_capacity*sizeof(LineVertex)),D3D11_USAGE_DYNAMIC,D3D11_BIND_VERTEX_BUFFER,D3D11_CPU_ACCESS_WRITE,0,0};winrt::check_hresult(d3d->CreateBuffer(&desc,nullptr,line_buffer.put()));}
        D3D11_MAPPED_SUBRESOURCE mapped{};winrt::check_hresult(d3dc->Map(line_buffer.get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped));memcpy(mapped.pData,line_vertices.data(),count*sizeof(LineVertex));d3dc->Unmap(line_buffer.get(),0);
        winrt::check_hresult(d3dc->Map(line_constants.get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped));const float size[8]{width,height,fade,bottom,left,right,side,0};memcpy(mapped.pData,size,sizeof(size));d3dc->Unmap(line_constants.get(),0);
        // Direct2D shares this device and leaves its own state behind: set everything the draw uses.
        const UINT stride=sizeof(LineVertex),offset=0;ID3D11Buffer* buffers[]{line_buffer.get()};ID3D11Buffer* cbs[]{line_constants.get()};
        d3dc->IASetInputLayout(line_layout.get());d3dc->IASetVertexBuffers(0,1,buffers,&stride,&offset);d3dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        d3dc->VSSetShader(line_vs.get(),nullptr,0);d3dc->VSSetConstantBuffers(0,1,cbs);d3dc->PSSetShader(line_ps.get(),nullptr,0);
        d3dc->GSSetShader(nullptr,nullptr,0);d3dc->HSSetShader(nullptr,nullptr,0);d3dc->DSSetShader(nullptr,nullptr,0);
        d3dc->RSSetState(line_raster.get());const D3D11_VIEWPORT viewport{0,0,width,height,0,1};d3dc->RSSetViewports(1,&viewport);
        const float factor[4]{};d3dc->OMSetBlendState(line_blend.get(),factor,0xffffffff);d3dc->OMSetDepthStencilState(nullptr,0);
        ID3D11RenderTargetView* views[]{view};d3dc->OMSetRenderTargets(1,views,nullptr);
        d3dc->Draw(count,0);
        ID3D11RenderTargetView* none[]{nullptr};d3dc->OMSetRenderTargets(1,none,nullptr);
        line_vertices.clear();
    }
    // Breaks up 8-bit banding over the whole target: about +-1.5/255 of noise, zero on average.
    void dither(ID3D11RenderTargetView* view,float width,float height){
        D3D11_MAPPED_SUBRESOURCE mapped{};
        d3dc->IASetInputLayout(nullptr);d3dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);d3dc->VSSetShader(dither_vs.get(),nullptr,0);d3dc->PSSetShader(dither_ps.get(),nullptr,0);
        d3dc->GSSetShader(nullptr,nullptr,0);d3dc->HSSetShader(nullptr,nullptr,0);d3dc->DSSetShader(nullptr,nullptr,0);
        d3dc->RSSetState(line_raster.get());const D3D11_VIEWPORT viewport{0,0,width,height,0,1};d3dc->RSSetViewports(1,&viewport);d3dc->OMSetDepthStencilState(nullptr,0);
        ID3D11RenderTargetView* views[]{view};d3dc->OMSetRenderTargets(1,views,nullptr);ID3D11Buffer* cbs[]{line_constants.get()};d3dc->PSSetConstantBuffers(0,1,cbs);
        const float factor[4]{};
        for(auto* state:{dither_add.get(),dither_take.get()}){
            dither_seed=std::fmod(dither_seed+37.31f,1000.f);
            winrt::check_hresult(d3dc->Map(line_constants.get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped));const float values[8]{width,height,0,dither_seed,0,0,0,0};memcpy(mapped.pData,values,sizeof(values));d3dc->Unmap(line_constants.get(),0);
            d3dc->OMSetBlendState(state,factor,0xffffffff);d3dc->Draw(3,0);
        }
        ID3D11RenderTargetView* none[]{nullptr};d3dc->OMSetRenderTargets(1,none,nullptr);
    }

    // A filled area under a line: each point's column from the line down (or up) to `to` y values,
    // coloured `near` at the line and `far` at the other end (premultiplied). Used for gradients.
    void add_fill(std::vector<dusk::WavePoint> const& points,std::vector<float> const& to,float scale,D2D1_COLOR_F near_colour,D2D1_COLOR_F far_colour){
        if(points.size()<2||to.size()!=points.size())return;
        const auto vertex=[&](float x,float y,D2D1_COLOR_F c){return LineVertex{x*scale,y*scale,0,1e5f,1,c.r,c.g,c.b,c.a};};
        for(size_t k=0;k+1<points.size();++k){
            const auto a=vertex(points[k].x,points[k].y,near_colour),b=vertex(points[k+1].x,points[k+1].y,near_colour),
                       c=vertex(points[k].x,to[k],far_colour),d=vertex(points[k+1].x,to[k+1],far_colour);
            line_vertices.insert(line_vertices.end(),{a,b,c,b,d,c});
        }
    }
    // A dashed (or dotted) line: dashes `dash` DIPs long, starting every `spacing` DIPs along the points.
    void add_dots(std::vector<dusk::WavePoint> const& points,float scale,float half,float r,float g,float b,float a,float spacing,float dash){
        if(points.size()<2)return;
        // Distances along the whole line, so a dash can run on across the line's short segments.
        std::vector<float> along(points.size(),0);
        for(size_t k=1;k<points.size();++k)along[k]=along[k-1]+std::hypot(points[k].x-points[k-1].x,points[k].y-points[k-1].y);
        const auto at=[&](float distance){
            const size_t k=std::min<size_t>(points.size()-2,static_cast<size_t>(std::upper_bound(along.begin(),along.end(),distance)-along.begin())-1);
            const float span=std::max(1e-4f,along[k+1]-along[k]),t=std::clamp((distance-along[k])/span,0.f,1.f);
            return dusk::WavePoint{points[k].x+(points[k+1].x-points[k].x)*t,points[k].y+(points[k+1].y-points[k].y)*t};};
        std::vector<dusk::WavePoint> piece;
        for(float start=0;start<along.back();start+=spacing){
            const float end=std::min(along.back(),start+dash);
            piece.clear();piece.push_back(at(start));
            for(size_t k=0;k<points.size();++k)if(along[k]>start&&along[k]<end)piece.push_back(points[k]);
            piece.push_back(at(end));
            add_line(piece,0,0,scale,half,r,g,b,a);
        }
    }
};

// Development (--stage-bench): saves a swap chain's back buffer as a BMP, to look at offscreen.
inline void save_chain_capture(IDXGISwapChain1* chain,ID3D11Device* d3d,ID3D11DeviceContext* d3dc,std::filesystem::path const& capture){
    winrt::com_ptr<ID3D11Texture2D> back;winrt::check_hresult(chain->GetBuffer(0,__uuidof(ID3D11Texture2D),back.put_void()));
    D3D11_TEXTURE2D_DESC desc{};back->GetDesc(&desc);desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;desc.MiscFlags=0;
    winrt::com_ptr<ID3D11Texture2D> copy;winrt::check_hresult(d3d->CreateTexture2D(&desc,nullptr,copy.put()));d3dc->CopyResource(copy.get(),back.get());
    D3D11_MAPPED_SUBRESOURCE mapped{};winrt::check_hresult(d3dc->Map(copy.get(),0,D3D11_MAP_READ,0,&mapped));
    BITMAPFILEHEADER file{};BITMAPINFOHEADER info{sizeof(info)};info.biWidth=static_cast<LONG>(desc.Width);info.biHeight=-static_cast<LONG>(desc.Height);info.biPlanes=1;info.biBitCount=32;
    const DWORD bytes=desc.Width*desc.Height*4;file.bfType=0x4D42;file.bfOffBits=sizeof(file)+sizeof(info);file.bfSize=file.bfOffBits+bytes;
    std::ofstream out(capture,std::ios::binary);out.write(reinterpret_cast<const char*>(&file),sizeof(file));out.write(reinterpret_cast<const char*>(&info),sizeof(info));
    for(UINT y=0;y<desc.Height;++y)out.write(static_cast<const char*>(mapped.pData)+static_cast<size_t>(y)*mapped.RowPitch,desc.Width*4);
    d3dc->Unmap(copy.get(),0);
}

