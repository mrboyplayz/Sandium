#include "CustomModels.hpp"

#include "Addresses.hpp"
#include "api/GLUniforms.hpp"
#include "api/Image.hpp"
#include "api/Logging.hpp"
#include "hooks/CreateItem.hpp"

#include <glad/glad.h>
#include <subhook.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace custommodels
{
    namespace
    {
        struct Vertex { float x, y, z, u, v, nx, ny, nz; };
        std::vector<Vertex> vertices;
        std::shared_ptr<api::Image> texture;
        GLuint vao = 0, vbo = 0, program = 0;
        GLint vpLocation = -1, modelLocation = -1, textureLocation = -1;
        int revolverType = -1;
        bool initialized = false;
        bool initFailed = false;
        bool spawned = false;
        bool wasInGame = false;
        int revolverBaseType = -1;
        std::string revolverTypeID = "sandium:revolver";
        std::array<bool, structs::Item::VanillaCount> revolverItems{};
        constexpr int revolverModelResource = 127;

        GLuint Compile(GLenum type, const char *source)
        {
            GLuint shader = glCreateShader(type);
            glShaderSource(shader, 1, &source, nullptr);
            glCompileShader(shader);
            GLint okay = 0;
            glGetShaderiv(shader, GL_COMPILE_STATUS, &okay);
            if (!okay) { glDeleteShader(shader); return 0; }
            return shader;
        }

        template<typename T> bool Read(std::ifstream &file, T &value)
        {
            return static_cast<bool>(file.read(reinterpret_cast<char *>(&value), sizeof(value)));
        }

        bool LoadCmo(const char *path)
        {
            std::ifstream file(path, std::ios::binary);
            char magic[4]{}; std::uint32_t version=0, vertexCount=0;
            if (!file.read(magic,4) || std::string(magic,4)!="CMod" || !Read(file,version) ||
                version!=3 || !Read(file,vertexCount) || vertexCount>1000000) return false;
            struct CmoVertex { float x,y,z,u,v,unused; };
            std::vector<CmoVertex> source(vertexCount);
            if (!file.read(reinterpret_cast<char *>(source.data()),source.size()*sizeof(CmoVertex))) return false;
            std::uint32_t faceCount=0; if(!Read(file,faceCount) || faceCount>1000000) return false;
            vertices.clear(); vertices.reserve(static_cast<std::size_t>(faceCount)*3);
            for(std::uint32_t face=0;face<faceCount;++face)
            {
                std::uint32_t count=0; if(!Read(file,count) || count!=3) return false;
                std::array<std::uint32_t,3> index{};
                if(!file.read(reinterpret_cast<char *>(index.data()),sizeof(index))) return false;
                std::uint32_t flags=0,material=0; if(!Read(file,flags)||!Read(file,material)) return false;
                for(auto i:index) if(i>=source.size()) return false;
                const auto &a=source[index[0]],&b=source[index[1]],&c=source[index[2]];
                const float abx=b.x-a.x,aby=b.y-a.y,abz=b.z-a.z;
                const float acx=c.x-a.x,acy=c.y-a.y,acz=c.z-a.z;
                float nx=aby*acz-abz*acy,ny=abz*acx-abx*acz,nz=abx*acy-aby*acx;
                const float length=std::sqrt(nx*nx+ny*ny+nz*nz); if(length>0){nx/=length;ny/=length;nz/=length;}
                for(auto i:index){const auto &v=source[i];vertices.push_back({v.x,v.y,v.z,v.u,v.v,nx,ny,nz});}
            }
            return !vertices.empty();
        }

        bool Initialize()
        {
            if (initialized) return true;
            if (initFailed || !api::glcap::HasViewProjection()) return false;
            if (!LoadCmo("sandium/models/revolver/revolver.cmo")) { api::GetSandiumLogger()->Log("Custom revolver error: could not parse revolver.cmo"); initFailed = true; return false; }
            try { texture = api::Image::Load("sandium/models/revolver/TheGenerals451Tex.png"); }
            catch (const std::exception &error) { api::GetSandiumLogger()->Log("Custom revolver texture error: {}", error.what()); initFailed = true; return false; }

            const char *vs = "#version 330 core\nlayout(location=0) in vec3 p; layout(location=1) in vec2 uv; layout(location=2) in vec3 n; uniform mat4 vp; uniform mat4 model; out vec2 tex; out float light; void main(){ vec3 normal=normalize(mat3(model)*n); light=0.30+0.70*max(dot(normal,normalize(vec3(0.3,0.8,0.5))),0.0); tex=uv; gl_Position=vp*model*vec4(p,1.0); }";
            const char *fs = "#version 330 core\nin vec2 tex; in float light; uniform sampler2D image; out vec4 color; void main(){ vec4 c=texture(image,tex); if(c.a<0.05) discard; color=vec4(c.rgb*light,c.a); }";
            GLuint v = Compile(GL_VERTEX_SHADER, vs), f = Compile(GL_FRAGMENT_SHADER, fs);
            if (!v || !f) { api::GetSandiumLogger()->Log("Custom revolver error: shader compilation failed"); initFailed = true; return false; }
            program = glCreateProgram(); glAttachShader(program,v); glAttachShader(program,f); glLinkProgram(program);
            glDeleteShader(v); glDeleteShader(f);
            GLint linked=0; glGetProgramiv(program,GL_LINK_STATUS,&linked);
            if (!linked) { api::GetSandiumLogger()->Log("Custom revolver error: shader link failed"); initFailed=true; return false; }
            vpLocation=glGetUniformLocation(program,"vp"); modelLocation=glGetUniformLocation(program,"model"); textureLocation=glGetUniformLocation(program,"image");
            glGenVertexArrays(1,&vao); glGenBuffers(1,&vbo); glBindVertexArray(vao); glBindBuffer(GL_ARRAY_BUFFER,vbo);
            glBufferData(GL_ARRAY_BUFFER,vertices.size()*sizeof(Vertex),vertices.data(),GL_STATIC_DRAW);
            glEnableVertexAttribArray(0); glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,sizeof(Vertex),(void*)0);
            glEnableVertexAttribArray(1); glVertexAttribPointer(1,2,GL_FLOAT,GL_FALSE,sizeof(Vertex),(void*)(3*sizeof(float)));
            glEnableVertexAttribArray(2); glVertexAttribPointer(2,3,GL_FLOAT,GL_FALSE,sizeof(Vertex),(void*)(5*sizeof(float)));
            glBindVertexArray(0); initialized=true;
            api::GetSandiumLogger()->Log("Custom revolver: loaded CMO with {} render vertices and texture {}", vertices.size(), texture->TextureID());
            return true;
        }

        void SpawnForPlayer()
        {
            if (spawned || revolverType < 0) return;
            for (std::size_t h=0; h<structs::Human::VanillaCount; ++h)
            {
                auto &human=addresses::Humans[h];
                if (!human.isActive.b1) continue;
                int slot=-1;
                for (int s=0;s<6;++s) if (human.inventorySlots[s].numberOfPlaces == 0) { slot=s; break; }
                if (slot < 0) return;
                structs::CVector3 pos=human.position, velocity{}; structs::COrientation orientation;
                int id=-1;
                // Creation validates against the 46 stock definitions. Create the
                // physical item through Magnum, then give it the separately
                // initialized Sandium type used by all subsequent item logic.
                { subhook::ScopedHookRemove remove(createItemHook); id=addresses::CreateItemFunc(revolverBaseType,&pos,&velocity,&orientation); }
                if (id < 0 || id >= static_cast<int>(structs::Item::VanillaCount)) return;
                auto &item=addresses::Items[id];
                item.typeID=revolverType;
                item.parentHumanID=static_cast<int>(h); item.parentInventorySlotID=slot; item.parentItemID=-1;
                item.isPhysicsEnabled=false; item.position=human.position; item.alternativePosition=human.position;
                human.inventorySlots[slot].firstPlaceItemID=id; human.inventorySlots[slot].numberOfPlaces=1;
                human.inventorySlots[slot].secondPlaceItemID=-1;
                revolverItems[id]=true;
                api::GetSandiumLogger()->Log("Custom revolver: spawned item {} for human {} in inventory slot {}", id, h, slot);
                spawned=true; return;
            }
        }

        void DrawItem(const structs::Item &item)
        {
            constexpr float scale=0.62f;
            const auto &o=item.orientation; const auto &p=item.position;
            const float model[16]={o.right.x*scale,o.right.y*scale,o.right.z*scale,0,
                                   o.up.x*scale,o.up.y*scale,o.up.z*scale,0,
                                   o.back.x*scale,o.back.y*scale,o.back.z*scale,0,
                                   p.x,p.y,p.z,1};
            glUniformMatrix4fv(modelLocation,1,GL_FALSE,model);
            glDrawArrays(GL_TRIANGLES,0,static_cast<GLsizei>(vertices.size()));
        }
    }

    void ConfigureRevolverType()
    {
        for (std::size_t i=0;i<structs::ItemType::VanillaCount;++i)
        {
            if (std::string(addresses::ItemTypes[i].name)=="Magnum")
                revolverBaseType=static_cast<int>(i);
        }
        if (revolverBaseType < 0) return;

        // Sandium identity 46 is deliberately virtual: Sub Rosa's inventory code
        // rejects native indices above 45. Instances retain valid gun mechanics
        // while this registry selects the separate model and texture.
        revolverType=static_cast<int>(structs::ItemType::VanillaCount);
        auto &definition=addresses::ItemTypes[revolverType];
        definition=addresses::ItemTypes[revolverBaseType];
        definition.customData.index=revolverType;
        definition.customData.typeIDPtr=&revolverTypeID;
        definition.SetName("Revolver");

#if _WIN32
        using LoadCmoResource = std::int64_t (*)(int, int, const char *);
        using BindItemModel = double (*)(int, int);
        const auto base=reinterpret_cast<std::uintptr_t>(addresses::Base.ptr);
        const auto load=reinterpret_cast<LoadCmoResource>(base+0xE9A60);
        const auto bind=reinterpret_cast<BindItemModel>(base+0x13FCD0);
        load(revolverModelResource,-1,"revolver");
        bind(revolverType,revolverModelResource);
#endif
        api::GetSandiumLogger()->Log("Registered custom item type sandium:revolver at index {} (gun mechanics from type {})", revolverType, revolverBaseType);
    }

    void UpdateAndDraw()
    {
        const bool inGame=addresses::IsInGame.ptr && *addresses::IsInGame;
        if (!inGame) { spawned=false; wasInGame=false; revolverItems.fill(false); return; }
        if (!wasInGame) { spawned=false; wasInGame=true; }
        SpawnForPlayer();
        return; // Native CMO binding renders the revolver; no overlay is needed.
        if (revolverType<0 || !Initialize()) return;
        GLint oldProgram=0,oldVao=0,oldTexture=0; glGetIntegerv(GL_CURRENT_PROGRAM,&oldProgram); glGetIntegerv(GL_VERTEX_ARRAY_BINDING,&oldVao); glGetIntegerv(GL_TEXTURE_BINDING_2D,&oldTexture);
        GLboolean depth=glIsEnabled(GL_DEPTH_TEST),blend=glIsEnabled(GL_BLEND),cull=glIsEnabled(GL_CULL_FACE);
        // Draw after the stock scene and ignore its depth buffer so the custom
        // replacement fully covers the engine's backing Magnum mesh.
        glDisable(GL_DEPTH_TEST); glEnable(GL_BLEND); glDisable(GL_CULL_FACE); glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
        glUseProgram(program); glUniformMatrix4fv(vpLocation,1,api::glcap::MatrixTransposed()?GL_TRUE:GL_FALSE,api::glcap::ViewProjectionMatrix()); glUniform1i(textureLocation,0);
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D,texture->TextureID()); glBindVertexArray(vao);
        static bool reported=false;
        for(std::size_t i=0;i<structs::Item::VanillaCount;++i)
        {
            if(revolverItems[i] && !addresses::Items[i].isActive.b1) revolverItems[i]=false;
            if(revolverItems[i] && addresses::Items[i].isActive.b1)
            {
                DrawItem(addresses::Items[i]);
                if(!reported)
                {
                    const auto &p=addresses::Items[i].position;
                    api::GetSandiumLogger()->Log("Custom revolver CMO renderer active: {} vertices, item {}, position ({:.2f}, {:.2f}, {:.2f})", vertices.size(), i, p.x, p.y, p.z);
                    reported=true;
                }
            }
        }
        glBindVertexArray(oldVao); glBindTexture(GL_TEXTURE_2D,oldTexture); glUseProgram(oldProgram);
        if(!depth) glDisable(GL_DEPTH_TEST); if(!blend) glDisable(GL_BLEND); if(cull) glEnable(GL_CULL_FACE);
    }

    void Shutdown()
    {
        texture.reset(); if(vbo) glDeleteBuffers(1,&vbo); if(vao) glDeleteVertexArrays(1,&vao); if(program) glDeleteProgram(program);
        vbo=vao=program=0; initialized=false; vertices.clear();
    }
}
