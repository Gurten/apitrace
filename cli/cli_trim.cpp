/**************************************************************************
 *
 * Copyright 2010 VMware, Inc.
 * Copyright 2011 Intel corporation
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 **************************************************************************/

#include <set>
#include <sstream>
#include <memory>
#include <map>
#include <type_traits>
#include <string.h>
#include <limits.h> // for CHAR_MAX
#include <getopt.h>


#include "cli.hpp"

#include "os_string.hpp"

#include "d3d9imports.hpp"
#include "d3d9size.hpp"

#include "trace_callset.hpp"
#include "trace_parser.hpp"
#include "trace_writer.hpp"


//TODO: Find new home
enum class ResourceType
{
    Unknown = 0,
    Texture,
};

enum class ResourceAction
{
    Unknown = 0,
    TextureLock,
    Memcpy,
    TextureUnlock,
};




class D3D9StateAggregator //TODO: extract interface
{
public:
    enum class CallCode
    {
        retrace_memcpy = 0,
        retrace_IUnknown__AddRef = 19 ,
        retrace_IUnknown__Release = 20 ,
        retrace_IUnknown__Release2 = 64 ,
        retrace_IUnknown__Release3 = 317,

        retrace_IUnknown__QueryInterface = 196,

        retrace_IDirect3DTexture9__GetSurfaceLevel = 80 ,

        retrace_IDirect3DTexture9__LockRect=  81 ,
        retrace_IDirect3DTexture9__UnlockRect = 82 ,

        retrace_IDirect3DVertexBuffer9__Lock = 150,
        retrace_IDirect3DVertexBuffer9__Unlock = 151,

        retrace_IDirect3DDevice9__TestCooperativeLevel=  199,
        retrace_IDirect3DDevice9__GetDirect3D=  202,
        retrace_IDirect3DDevice9__Present = 213,
        retrace_IDirect3DDevice9__CreateTexture=  219,


        retrace_IDirect3DDevice9__CreateVertexBuffer = 222,
        retrace_IDirect3DDevice9__SetViewport = 243,
        retrace_IDirect3DDevice9__SetRenderState = 253,
        retrace_IDirect3DDevice9__SetTexture = 261,
        retrace_IDirect3DDevice9__SetSamplerState = 265,
        retrace_IDirect3DDevice9__CreateVertexDeclaration = 282,
        retrace_IDirect3DDevice9__SetVertexDeclaration = 283,
        retrace_IDirect3DDevice9__CreateVertexShader = 287,
        retrace_IDirect3DDevice9__SetVertexShader = 288,
        retrace_IDirect3DDevice9__SetVertexShaderConstantF = 290,
        retrace_IDirect3DDevice9__SetStreamSource = 296,
        retrace_IDirect3DDevice9__CreatePixelShader = 302,
        retrace_IDirect3DDevice9__SetPixelShader = 303,
        retrace_IDirect3DDevice9__SetPixelShaderConstantF = 305,
        retrace_IDirect3D9__CreateDevice = 331,
        retrace_Direct3DCreate9 = 559,

    };

    struct MappedRegion
    {
        size_t size = 0;
        uint32_t resource = 0;
    };

private:


    class Resource
    {
    public: 
        Resource(std::unique_ptr<trace::Call> && call,
            std::map<uint32_t, MappedRegion> *activeRegions,
            ResourceType resourceType = ResourceType::Unknown)
            : creation(std::move(call))
            , activeRegions_(activeRegions)
            , resourceType_(resourceType) {}

        void addRef()
        {
            refcountCheck();

            ref_count_++;
        }

        void release()
        {
            refcountCheck();
            ref_count_--;
        }

        void addCall(std::unique_ptr<trace::Call> && call, ResourceAction resourceAction)
        {
            switch (resourceType_)
            {
            case ResourceType::Texture:
                handleCallAsTexture(std::move(call), resourceAction);
                break;

            default:
                std::cerr << "unsupported resource" << std::endl;
            
            }
        }

        std::vector< std::shared_ptr<trace::Call>> flatten() {
            std::vector< std::shared_ptr<trace::Call>> result;
            
            refcountCheck();

            if(ref_count_ <= 0)
            { 
                return {};
            }

            result.emplace_back(creation);

            for (auto& m : modifiers_)
            {
                result.insert(result.end(), m.second.begin(), m.second.end() );
            }

            return result;
        }

    private:
        void refcountCheck()
        {
            if (ref_count_ <= 0)
            {
                std::cerr << "invalid resource refcount encountered: 0x" 
                    << std::hex << creation->arg(7).toUInt() << std::dec << std::endl;
            }
        }

        void handleCallAsTexture(std::unique_ptr<trace::Call> && call, ResourceAction resourceAction)
        {
            switch (resourceAction)
            {
            case ResourceAction::TextureLock:
            {
                ++memory_mapped_region_count_;
                uint32_t subresource_index = (call->arg(1)).toUInt();
                auto sub_staging = staging_modifiers_.find(subresource_index);

                if (sub_staging != staging_modifiers_.cend()) {
                    if (sub_staging->second.size() > 0)
                    {
                        std::cerr << "throwing away texture operations" << std::endl;
                    }
                    sub_staging->second = { std::shared_ptr(std::move(call)) };
                }
                else
                {  
                    staging_modifiers_.emplace(subresource_index,
                        std::vector<std::shared_ptr<trace::Call>>({ std::shared_ptr(std::move(call)) }));
                }

                activeRegions_;


                break;
            }
                
            case ResourceAction::Memcpy:
            {
                //TODO: verify memcpy
                uint32_t subresource_index = (call->arg(1)).toUInt();
                auto sub_staging = staging_modifiers_.find(subresource_index);

                if (sub_staging != staging_modifiers_.cend()) {
                    sub_staging->second.emplace_back(std::move(call));
                }
                else
                {
                    std::cerr << "memcpy for unmapped region" << std::endl;
                }
                break;
            }
            case ResourceAction::TextureUnlock:
                --memory_mapped_region_count_;
                break;

            default:
                std::cerr << "unsupported resource action" << std::endl;
            }
        }


        int ref_count_ = 1;

        int memory_mapped_region_count_ = 0;
        ResourceType resourceType_;
        std::shared_ptr < trace::Call> creation;

        //subresource id to vector of modifying calls.
        std::map<uint32_t, std::vector< std::shared_ptr<trace::Call>>> modifiers_;

        std::map<uint32_t, std::vector< std::shared_ptr<trace::Call>>> staging_modifiers_;

        //
        std::map<uint32_t, MappedRegion> const *activeRegions_;
    };

public:
    

    D3D9StateAggregator() {}

    void addCall(std::unique_ptr<trace::Call> && call) {
        CallCode const call_code = static_cast<CallCode>(call->sig->id);
        switch (call_code)
        {
        
        case CallCode::retrace_memcpy           :
        case CallCode::retrace_IUnknown__AddRef :
        case CallCode::retrace_IUnknown__Release:
        case CallCode::retrace_IUnknown__Release2:
        case CallCode::retrace_IUnknown__Release3:

        case CallCode::retrace_IUnknown__QueryInterface:

        case CallCode::retrace_IDirect3DTexture9__GetSurfaceLevel:

        case CallCode::retrace_IDirect3DTexture9__LockRect:
        {
            auto it = resources_.find(call->arg(0).toUInt());
            if (it != resources_.cend())
            {
                it->second.addCall(std::move(call), ResourceAction::TextureLock);
            }
            else
            {
                std::cerr << "ERROR: trying to lock nonexistant texture." << std::endl;
            }
            break;
        }
        case CallCode::retrace_IDirect3DTexture9__UnlockRect:
            return; //Possibly have better lineage control for this
        case CallCode::retrace_IDirect3DVertexBuffer9__Lock:
        case CallCode::retrace_IDirect3DVertexBuffer9__Unlock:
        
        case CallCode::retrace_IDirect3DDevice9__TestCooperativeLevel:
        case CallCode::retrace_IDirect3DDevice9__GetDirect3D:
        case CallCode::retrace_IDirect3DDevice9__Present:
        case CallCode::retrace_IDirect3DDevice9__CreateTexture:
        {
            auto[_, success_inserted] = resources_.emplace(call->arg(7).toUInt(), 
                Resource(std::move(call), &activeRegions_,  ResourceType::Texture));
            if (!success_inserted)
            {
                std::cerr << "ERROR: texture already created." << std::endl;
            }
            break;
        }
        case CallCode::retrace_IDirect3DDevice9__CreateVertexBuffer     :
        case CallCode::retrace_IDirect3DDevice9__SetViewport            :
        case CallCode::retrace_IDirect3DDevice9__SetRenderState         :
        case CallCode::retrace_IDirect3DDevice9__SetTexture             :
        case CallCode::retrace_IDirect3DDevice9__SetSamplerState        :
        case CallCode::retrace_IDirect3DDevice9__CreateVertexDeclaration:
        case CallCode::retrace_IDirect3DDevice9__SetVertexDeclaration   :
        case CallCode::retrace_IDirect3DDevice9__CreateVertexShader     :
        case CallCode::retrace_IDirect3DDevice9__SetVertexShader        :
        case CallCode::retrace_IDirect3DDevice9__SetVertexShaderConstantF:
        case CallCode::retrace_IDirect3DDevice9__SetStreamSource        :
        case CallCode::retrace_IDirect3DDevice9__CreatePixelShader      :
        case CallCode::retrace_IDirect3DDevice9__SetPixelShader         :
        case CallCode::retrace_IDirect3DDevice9__SetPixelShaderConstantF:
        
        case CallCode::retrace_IDirect3D9__CreateDevice:
        case CallCode::retrace_Direct3DCreate9:

        
        default:
            break;
        }
    
    }

    std::vector< std::shared_ptr<trace::Call>> getSquashedCalls() { return {}; }

private:
    std::map<uint32_t, Resource> resources_;

    std::map<uint32_t, MappedRegion> activeRegions_;
};


//==========================================^Find new home ^====================











static const char *synopsis = "Create a new trace by trimming an existing trace.";

static void
usage(void)
{
    std::cout
        << "usage: apitrace trim [OPTIONS] TRACE_FILE...\n"
        << synopsis << "\n"
        "\n"
        "    -h, --help               Show detailed help for trim options and exit\n"
        "        --calls=CALLSET      Include specified calls in the trimmed output.\n"
        "        --frames=FRAMESET    Include specified frames in the trimmed output.\n"
        "        --thread=THREAD_ID   Only retain calls from specified thread (can be passed multiple times.)\n"
        "    -o, --output=TRACE_FILE  Output trace file\n"
    ;
}

enum {
    CALLS_OPT = CHAR_MAX + 1,
    FRAMES_OPT,
    THREAD_OPT,
    SQUASH_OPT,

};

const static char *
shortOptions = "aho:x";

const static struct option
longOptions[] = {
    {"help", no_argument, 0, 'h'},
    {"calls", required_argument, 0, CALLS_OPT},
    {"frames", required_argument, 0, FRAMES_OPT},
    {"thread", required_argument, 0, THREAD_OPT},
    {"squash-until-frame", required_argument, 0, SQUASH_OPT},
    {"output", required_argument, 0, 'o'},
    {0, 0, 0, 0}
};

struct stringCompare {
    bool operator() (const char *a, const char *b) const {
        return strcmp(a, b) < 0;
    }
};

struct trim_options {
    /* Calls to be included in trace. */
    trace::CallSet calls;

    /* Frames to be included in trace. */
    trace::CallSet frames;

    /* Output filename */
    std::string output;

    /*Attempt to follow lineage of resource updates for individual resources 
     until this frame*/
    unsigned int squash_until_frame;

    /* Emit only calls from this thread (empty == all threads) */
    std::set<unsigned> threadIds;
};

static int
trim_trace(const char *filename, struct trim_options *options)
{
    trace::Parser p;
    unsigned frame;

    if (!p.open(filename)) {
        std::cerr << "error: failed to open " << filename << "\n";
        return 1;
    }

    /* Prepare output file and writer for output. */
    if (options->output.empty()) {
        os::String base(filename);
        base.trimExtension();

        options->output = std::string(base.str()) + std::string("-trim.trace");
    }

    trace::Writer writer;
    if (!writer.open(options->output.c_str(), p.getVersion(), p.getProperties())) {
        std::cerr << "error: failed to create " << options->output << "\n";
        return 1;
    }

    //TODO: get the api name/version from the file p. Only planned support for D3D9.
    D3D9StateAggregator state_aggregator;

    frame = 0;
    std::unique_ptr<trace::Call> call;


    const unsigned int squash_until_frame = options->squash_until_frame;
    
    while (frame < squash_until_frame) {
        call = std::unique_ptr<trace::Call>(p.parse_call());
        if (!call)
        {
            break;
        }
        
        trace::CallFlags const& call_flags = call->flags;
        state_aggregator.addCall(std::move(call));
        if (call_flags & trace::CALL_FLAG_END_FRAME) {
            frame++;
        }
    }

    for (auto& squash_call : state_aggregator.getSquashedCalls())
    {
        writer.writeCall(squash_call.get());
    }


    while ((call = std::unique_ptr<trace::Call>(p.parse_call()))) {
        trace::CallFlags const& call_flags = call->flags;

        /* There's no use doing any work past the last call and frame
         * requested by the user. */
        if ((options->calls.empty() || call->no > options->calls.getLast()) &&
            (options->frames.empty() || frame > options->frames.getLast())) {

            break;
        }

        /* If requested, ignore all calls not belonging to the specified thread. */
        if (!options->threadIds.empty() &&
            options->threadIds.find(call->thread_id) == options->threadIds.end()) {
            goto NEXT;
        }

        /* If this call is included in the user-specified call set,
         * then require it (and all dependencies) in the trimmed
         * output. */
        if (options->calls.contains(*call) ||
            options->frames.contains(frame, call_flags)) {

            writer.writeCall(call.get());
        }

    NEXT:
        if (call_flags & trace::CALL_FLAG_END_FRAME) {
            frame++;
        }

    }

    std::cerr << "Trimmed trace is available as " << options->output << "\n";

    return 0;
}

static int
command(int argc, char *argv[])
{
    struct trim_options options;

    options.calls = trace::CallSet(trace::FREQUENCY_NONE);
    options.frames = trace::CallSet(trace::FREQUENCY_NONE);

    int opt;
    while ((opt = getopt_long(argc, argv, shortOptions, longOptions, NULL)) != -1) {
        switch (opt) {
        case 'h':
            usage();
            return 0;
        case CALLS_OPT:
            options.calls.merge(optarg);
            break;
        case FRAMES_OPT:
            options.frames.merge(optarg);
            break;
        case SQUASH_OPT:
            options.squash_until_frame = atoi(optarg);
            break;
        case THREAD_OPT:
            options.threadIds.insert(atoi(optarg));
            break;
        case 'o':
            options.output = optarg;
            break;
        default:
            std::cerr << "error: unexpected option `" << (char)opt << "`\n";
            usage();
            return 1;
        }
    }

    /* If neither of --calls nor --frames was set, default to the
     * entire set of calls. */
    if (options.calls.empty() && options.frames.empty()) {
        options.calls = trace::CallSet(trace::FREQUENCY_ALL);
    }

    if (optind >= argc) {
        std::cerr << "error: apitrace trim requires a trace file as an argument.\n";
        usage();
        return 1;
    }

    if (argc > optind + 1) {
        std::cerr << "error: extraneous arguments:";
        for (int i = optind + 1; i < argc; i++) {
            std::cerr << " " << argv[i];
        }
        std::cerr << "\n";
        usage();
        return 1;
    }

    return trim_trace(argv[optind], &options);
}

const Command trim_command = {
    "trim",
    synopsis,
    usage,
    command
};
