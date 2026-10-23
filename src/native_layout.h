#pragma once
// Conservative MSVC x64 layout discovery. No native function is called here.
// A matching string alone is insufficient: PE unwind ranges, RTTI, packet
// copies, constructor stores, and the encoder's reads must all agree.
#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace eye::native {
struct Layout {
    uint32_t receive=0,present=0,hmdVtable=0,presentThis=0,providerHmd=0;
    uint32_t owner=0,session=0,gaze=0,timestamp=0,marker=0,eyeEnabled=0;
    uint32_t spatialMode=0,crop=0,cropSize=0,uv=0;
};
struct Range { uint32_t begin=0,end=0; };
class Image {
    const uint8_t* bytes;size_t size;uintptr_t address;
    std::vector<Range> code,rodata,functions;
    std::vector<uint32_t> roots;
    std::map<uint32_t,Range> canonical;
    std::set<uint32_t> fragmented;
public:
    Image(const uint8_t* data,size_t length,uintptr_t imageAddress):bytes(data),size(length),address(imageAddress){}
    bool Contains(uint32_t rva,size_t count=1)const{return rva<=size&&count<=size-rva;}
    template<class T>T Read(uint32_t rva)const{T value{};if(Contains(rva,sizeof(T)))memcpy(&value,bytes+rva,sizeof(T));return value;}
    bool Match(uint32_t rva,std::initializer_list<int> pattern)const {
        if(!Contains(rva,pattern.size()))return false;
        for(int value:pattern){if(value>=0&&bytes[rva]!=value)return false;++rva;}return true;
    }
    std::vector<uint32_t> Find(Range range,std::initializer_list<int> pattern)const {
        std::vector<uint32_t> found;if(range.end<range.begin||pattern.size()>range.end-range.begin)return found;
        for(uint32_t i=range.begin;i<=range.end-pattern.size();++i)if(Match(i,pattern))found.push_back(i);
        return found;
    }
    uint32_t Unique(Range range,std::initializer_list<int> pattern)const {
        auto found=Find(range,pattern);return found.size()==1?found[0]:0;
    }
    bool InCode(uint32_t rva)const {for(auto r:code)if(rva>=r.begin&&rva<r.end)return true;return false;}
    bool InRodata(uint32_t rva)const {for(auto r:rodata)if(rva>=r.begin&&rva<r.end)return true;return false;}
    uint32_t Rva(uintptr_t va)const{return va>=address&&va-address<size?uint32_t(va-address):0;}
    Range Function(uint32_t rva)const {
        auto i=std::upper_bound(functions.begin(),functions.end(),rva,[](uint32_t x,Range y){return x<y.begin;});
        if(i!=functions.begin()){--i;if(rva<i->end){auto root=roots[size_t(i-functions.begin())];
            auto f=canonical.find(root);if(f!=canonical.end()&&!fragmented.count(root))return f->second;}}
        return {};
    }
    bool Parse() {
        if(size<sizeof(IMAGE_DOS_HEADER)||Read<uint16_t>(0)!=IMAGE_DOS_SIGNATURE)return false;
        auto nt=Read<uint32_t>(offsetof(IMAGE_DOS_HEADER,e_lfanew));
        if(!Contains(nt,sizeof(IMAGE_NT_HEADERS64)))return false;
        auto h=Read<IMAGE_NT_HEADERS64>(nt);
        if(h.Signature!=IMAGE_NT_SIGNATURE||h.FileHeader.Machine!=IMAGE_FILE_MACHINE_AMD64||
           h.OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC||h.OptionalHeader.SizeOfImage!=size||
           h.FileHeader.SizeOfOptionalHeader!=sizeof(IMAGE_OPTIONAL_HEADER64)||h.FileHeader.NumberOfSections>96)return false;
        auto sections=nt+4+sizeof(IMAGE_FILE_HEADER)+h.FileHeader.SizeOfOptionalHeader;
        if(!Contains(sections,size_t(h.FileHeader.NumberOfSections)*sizeof(IMAGE_SECTION_HEADER)))return false;
        for(unsigned i=0;i<h.FileHeader.NumberOfSections;++i){
            auto s=Read<IMAGE_SECTION_HEADER>(sections+i*uint32_t(sizeof(IMAGE_SECTION_HEADER)));
            if(!Contains(s.VirtualAddress,s.Misc.VirtualSize))return false;
            Range r{s.VirtualAddress,s.VirtualAddress+s.Misc.VirtualSize};
            if(s.Characteristics&IMAGE_SCN_MEM_EXECUTE)code.push_back(r);
            else if((s.Characteristics&IMAGE_SCN_MEM_READ)&&!(s.Characteristics&IMAGE_SCN_MEM_WRITE))rodata.push_back(r);
        }
        auto unwind=h.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
        if(!unwind.Size||unwind.Size%12||!Contains(unwind.VirtualAddress,unwind.Size))return false;
        for(uint32_t i=0;i<unwind.Size;i+=12){
            auto a=Read<uint32_t>(unwind.VirtualAddress+i),b=Read<uint32_t>(unwind.VirtualAddress+i+4);
            if(a>=b||!InCode(a)||!InCode(b-1)||(!functions.empty()&&a<functions.back().end))return false;
            auto root=a,unwindRva=Read<uint32_t>(unwind.VirtualAddress+i+8);bool resolved=false;
            // Optimized MSVC splits a logical function into chained unwind
            // ranges. Anchors may be in a later range, not at the ABI entry.
            for(unsigned depth=0;depth<16;++depth){
                if(!Contains(unwindRva,4))return false;
                auto flags=Read<uint8_t>(unwindRva)>>3;
                if(!(flags&4)){resolved=true;break;}
                auto count=Read<uint8_t>(unwindRva+2);
                auto chain=unwindRva+4+((uint32_t(count)+1)&~1u)*2;
                if(!Contains(chain,12))return false;
                root=Read<uint32_t>(chain);unwindRva=Read<uint32_t>(chain+8);
                if(!InCode(root))return false;
            }
            if(!resolved)return false;
            functions.push_back({a,b});roots.push_back(root);
            auto found=canonical.find(root);
            if(found==canonical.end())canonical[root]={a,b};
            else {if(found->second.end!=a)fragmented.insert(root);found->second.end=b;}
        }
        return !code.empty()&&!rodata.empty()&&!functions.empty();
    }
    Range AnchoredFunction(const char* text)const {
        std::set<uint32_t> targets,starts;
        size_t length=strlen(text)+1;
        for(auto r:rodata)for(uint32_t i=r.begin;i+length<=r.end;++i)
            if(!memcmp(bytes+i,text,length))targets.insert(i);
        for(auto r:code)for(uint32_t i=r.begin;i+7<=r.end;++i){
            if((bytes[i]!=0x48&&bytes[i]!=0x4c)||bytes[i+1]!=0x8d||(bytes[i+2]&0xc7)!=0x05)continue;
            auto target=int64_t(i)+7+Read<int32_t>(i+3);
            if(target>0&&targets.count(uint32_t(target))){auto f=Function(i);if(f.end)starts.insert(f.begin);}
        }
        return starts.size()==1?Function(*starts.begin()):Range{};
    }
    std::vector<uint32_t> FindCode(std::initializer_list<int> pattern)const {
        std::vector<uint32_t> result;for(auto r:code){auto hits=Find(r,pattern);result.insert(result.end(),hits.begin(),hits.end());}return result;
    }
    bool Vtables(uint32_t present,Layout& l)const {
        std::set<uint32_t> primary,offsets;
        for(auto r:rodata)for(uint32_t i=(r.begin+7)&~7u;i+8<=r.end;i+=8){
            auto col=Rva(Read<uintptr_t>(i));
            if(!col||!Contains(col,24)||Read<uint32_t>(col)!=1||Read<uint32_t>(col+8)!=0||Read<uint32_t>(col+20)!=col)continue;
            auto type=Read<uint32_t>(col+12),offset=Read<uint32_t>(col+4);
            const char* name=".?AVSVLHMDDriverD3D11@@";
            if(!Contains(type+16,strlen(name)+1)||memcmp(bytes+type+16,name,strlen(name)+1))continue;
            if(offset==0&&InCode(Rva(Read<uintptr_t>(i+8))))primary.insert(i+8);
            if(offset>0&&offset<=64&&offset%8==0){
                for(unsigned slot=0;slot<16&&i+16+slot*8<=r.end;++slot){
                    auto fn=Rva(Read<uintptr_t>(i+8+slot*8));if(!InCode(fn))break;
                    if(fn==present)offsets.insert(offset);
                }
            }
        }
        if(primary.size()!=1||offsets.size()!=1)return false;
        l.hmdVtable=*primary.begin();l.presentThis=*offsets.begin();return true;
    }
    bool Resolve(Layout& result,std::string& why)const {
        Layout l;
        auto fail=[&](const char* message){why=message;return false;};
        auto rx=AnchoredFunction("/sl/eyeTrackedGazePoint"),present=AnchoredFunction("SVLHMDDriver::Present");
        if(!rx.end||!present.end)return fail("missing or ambiguous receive/Present anchors");
        l.receive=rx.begin;l.present=present.begin;
        if(!Vtables(l.present,l))return fail("RTTI/Present subobject mismatch");
        auto copy=Unique(rx,{0x49,0x89,0x86,-1,-1,-1,-1,0xf3,0x0f,0x10,0x4e,8,0xf3,0x41,0x0f,0x11,0x8e,-1,-1,-1,-1,
                            0xf3,0x0f,0x10,0x56,12,0xf3,0x41,0x0f,0x11,0x96,-1,-1,-1,-1,0xf3,0x0f,0x10,0x5e,16,0xf3,0x41,0x0f,0x11,0x9e,-1,-1,-1,-1});
        if(!copy)return fail("eye packet coordinate copy mismatch");
        if(!Unique({rx.begin,copy},{0x4c,0x8b,0xf1})||!Unique({rx.begin,copy},{0x48,0x8b,0xf2}))return fail("receiver this/packet register ABI mismatch");
        if(!Unique({rx.begin,copy},{0x80,0x7e,0x15,0,0x0f,0x84,-1,-1,-1,-1}))return fail("eye packet validity gate mismatch");
        l.timestamp=Read<uint32_t>(copy+3);l.gaze=Read<uint32_t>(copy+17);
        if(Read<uint32_t>(copy+31)!=l.gaze+4||Read<uint32_t>(copy+45)!=l.gaze+8||l.timestamp!=l.gaze+16)return fail("noncontiguous native gaze state");
        auto gate=Unique(present,{0x49,0x2b,0x87,-1,-1,-1,-1,0x41,0x80,0xbf,-1,-1,-1,-1,0,0x74,-1,0x48,0x3d,0x90,0xd0,3,0});
        if(!gate||Read<uint32_t>(gate+3)+l.presentThis!=l.timestamp)return fail("encoder timestamp/timeout mismatch");
        l.eyeEnabled=Read<uint32_t>(gate+10)+l.presentThis;
        auto z=Unique({gate,std::min(present.end,gate+100)}, {0x41,0x0f,0x2f,0x87,-1,-1,-1,-1});
        if(!z||Read<uint32_t>(z+4)+l.presentThis!=l.gaze+8)return fail("encoder gaze read mismatch");
        auto marker=Unique(present,{0x41,0x80,0xbf,-1,-1,-1,-1,0,0x74,0x0d,0xb8,0,1,0,0,0x66,0x41,0x09,0x87,-1,-1,-1,-1});
        if(!marker)return fail("native marker flag mismatch");
        l.marker=Read<uint32_t>(marker+3)+l.presentThis;
        auto mode=Unique(present,{0x41,0x80,0xbf,-1,-1,-1,-1,2,0x0f,0x85,-1,-1,-1,-1,0x41,0xc6,0x87,-1,-1,-1,-1,2});
        if(!mode)return fail("spatial encoding layout mismatch");
        l.spatialMode=Read<uint32_t>(mode+3)+l.presentThis;l.crop=Read<uint32_t>(mode+17)+l.presentThis;
        auto uv=Unique(present,{0xf3,0x41,0x0f,0x11,0xbf,-1,-1,-1,-1,0xf3,0x41,0x0f,0x11,0x8f,-1,-1,-1,-1,
                               0xf3,0x41,0x0f,0x11,0xa7,-1,-1,-1,-1,0xf3,0x41,0x0f,0x11,0x97,-1,-1,-1,-1,0x45,0x8b,0x87,-1,-1,-1,-1});
        if(!uv)return fail("projected UV/crop size mismatch");
        l.uv=Read<uint32_t>(uv+5)+l.presentThis;l.cropSize=Read<uint32_t>(uv+39)+l.presentThis;
        for(unsigned i=1;i<4;++i)if(Read<uint32_t>(uv+5+i*9)+l.presentThis!=l.uv+i*4)return fail("UV array layout mismatch");
        auto stores=Unique(present,{0x66,0x41,0x89,0xbf,-1,-1,-1,-1,0x41,0x8b,0xfc,0x66,0x45,0x89,0x97,-1,-1,-1,-1,
                                   0x66,0x45,0x89,0x9f,-1,-1,-1,-1,0x66,0x41,0x89,0x9f,-1,-1,-1,-1});
        if(!stores||Read<uint32_t>(stores+4)+l.presentThis!=l.crop+14||Read<uint32_t>(stores+15)+l.presentThis!=l.crop+4||
           Read<uint32_t>(stores+23)+l.presentThis!=l.crop+6||Read<uint32_t>(stores+31)+l.presentThis!=l.crop+12)return fail("crop rectangle stores mismatch");
        // Dimension reads independently tie the metadata array to the crop math.
        auto full=Unique({uv,present.end},{0x41,0x0f,0xbf,0x97,-1,-1,-1,-1,0xd1,0xf8,0x44,0x8b,0xd0});
        if(!full||Read<uint32_t>(full+4)+l.presentThis!=l.crop+8)return fail("crop dimensions mismatch");
        std::set<uint32_t> dispatches;
        for(auto at:FindCode({0xe8,-1,-1,-1,-1}))if(int64_t(at)+5+Read<int32_t>(at+1)==l.receive){auto f=Function(at);if(f.end&&f.begin!=rx.begin)dispatches.insert(f.begin);}
        if(dispatches.size()!=1)return fail("missing or ambiguous eye packet dispatcher");
        auto dispatch=Function(*dispatches.begin());
        if(!Unique(dispatch,{0x3c,0x11,0x0f,0x85,-1,-1,-1,-1}))return fail("dispatcher packet kind mismatch");
        auto ownerHmd=Unique(dispatch,{0x4c,0x8b,0xb5,-1,-1,-1,-1,0x4d,0x85,0xf6});
        if(!ownerHmd)return fail("provider HMD field mismatch");l.providerHmd=Read<uint32_t>(ownerHmd+3);
        // Base constructor stores the provider argument and clears the session.
        auto owners=FindCode({0x48,0x89,0x9f,-1,-1,-1,-1,0x4c,0x89,0xb7,-1,-1,-1,-1,0x44,0x89,0xb7,-1,-1,-1,-1});
        std::vector<uint32_t> constructors;
        for(auto at:owners){auto f=Function(at);if(!f.end)continue;
            if(!Unique(f,{0x48,0x8b,0xda})||!Unique(f,{0x4c,0x89,0xb7,int(l.gaze&255),int((l.gaze>>8)&255),int((l.gaze>>16)&255),int(l.gaze>>24)}))continue;
            if(Read<uint32_t>(at+10)!=Read<uint32_t>(at+3)+8||Read<uint32_t>(at+17)!=Read<uint32_t>(at+3)+16)continue;
            constructors.push_back(at);
        }
        if(constructors.size()!=1)return fail("constructor owner/session mismatch");
        l.owner=Read<uint32_t>(constructors[0]+3);l.session=Read<uint32_t>(constructors[0]+10);
        for(auto field:{l.providerHmd,l.owner,l.session,l.gaze,l.timestamp,l.marker,l.eyeEnabled,l.spatialMode,l.crop,l.cropSize,l.uv})
            if(field<0x20||field>0x2000)return fail("discovered field outside conservative object bounds");
        if(l.marker!=l.eyeEnabled+2||l.uv!=l.timestamp+8)return fail("native flag/state relationship mismatch");
        result=l;why="structural MSVC x64 match";return true;
    }
};
} // namespace eye::native
