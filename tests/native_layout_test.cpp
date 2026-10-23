#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <fstream>
#include <iostream>
#include <filesystem>
#include <stdexcept>
#include "../src/native_layout.h"

using Bytes=std::vector<uint8_t>;
template<class T>T Read(const Bytes& b,size_t at){T x{};if(at+sizeof(x)>b.size())throw std::runtime_error("fixture read bounds");memcpy(&x,b.data()+at,sizeof(x));return x;}
template<class T>void Write(Bytes& b,size_t at,T x){if(at+sizeof(x)>b.size())throw std::runtime_error("fixture write bounds");memcpy(b.data()+at,&x,sizeof(x));}
static void Check(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
static bool Resolve(const Bytes& b,uintptr_t base,eye::native::Layout& l,std::string& why){eye::native::Image image(b.data(),b.size(),base);return image.Parse()&&image.Resolve(l,why);}
static Bytes Load(const wchar_t* path,uintptr_t& base){
    std::ifstream in(std::filesystem::path(path),std::ios::binary);Check(bool(in),"open driver fixture");
    Bytes file((std::istreambuf_iterator<char>(in)),{});
    auto nt=Read<uint32_t>(file,0x3c);auto header=Read<IMAGE_NT_HEADERS64>(file,nt);
    Check(header.Signature==IMAGE_NT_SIGNATURE&&header.OptionalHeader.SizeOfImage<512*1024*1024,"valid fixture PE");
    Bytes b(header.OptionalHeader.SizeOfImage);base=uintptr_t(header.OptionalHeader.ImageBase);
    Check(header.OptionalHeader.SizeOfHeaders<=file.size(),"fixture headers");memcpy(b.data(),file.data(),header.OptionalHeader.SizeOfHeaders);
    auto sections=nt+4+sizeof(IMAGE_FILE_HEADER)+header.FileHeader.SizeOfOptionalHeader;
    for(unsigned i=0;i<header.FileHeader.NumberOfSections;++i){auto s=Read<IMAGE_SECTION_HEADER>(file,sections+i*sizeof(IMAGE_SECTION_HEADER));
        Check(size_t(s.PointerToRawData)+s.SizeOfRawData<=file.size()&&size_t(s.VirtualAddress)+s.SizeOfRawData<=b.size(),"fixture section bounds");
        memcpy(b.data()+s.VirtualAddress,file.data()+s.PointerToRawData,s.SizeOfRawData);
    }return b;
}
// Move the REAL handler and Present bodies, and fix only the image relationships
// used by discovery. This image is never loaded or executed. The test proves
// that old RVAs cannot explain a passing result, rather than testing a toy regex.
static Bytes MoveFunctions(const Bytes& original,uintptr_t base,const eye::native::Layout& l,bool duplicate=false){
    Bytes b=original;auto nt=Read<uint32_t>(b,0x3c);auto h=Read<IMAGE_NT_HEADERS64>(b,nt);
    auto table=h.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    std::vector<IMAGE_RUNTIME_FUNCTION_ENTRY> records;
    for(uint32_t at=table.VirtualAddress;at<table.VirtualAddress+table.Size;at+=12)records.push_back(Read<IMAGE_RUNTIME_FUNCTION_ENTRY>(b,at));
    eye::native::Image view(original.data(),original.size(),base);Check(view.Parse(),"parse before move");
    uint32_t start=uint32_t(b.size()),next=start;
    for(uint32_t fn:{l.receive,l.present}){
        auto i=std::find_if(records.begin(),records.end(),[&](auto x){return x.BeginAddress==fn;});Check(i!=records.end(),"move unwind range");
        auto record=*i;auto logical=view.Function(fn);auto length=logical.end-fn;auto dest=next;next=(dest+length+0xfff)&~0xfffu;b.resize(next);
        memcpy(b.data()+dest,original.data()+fn,length);
        for(uint32_t j=0;j+7<length;++j){auto at=dest+j;
            if((b[at]==0x48||b[at]==0x4c)&&b[at+1]==0x8d&&(b[at+2]&0xc7)==5){
                auto target=int64_t(fn+j)+7+Read<int32_t>(original,fn+j+3);
                if(target>0&&size_t(target)<original.size())Write<int32_t>(b,at+3,int32_t(target-(int64_t(at)+7)));
            }
        }
        if(!duplicate)std::fill(b.begin()+fn,b.begin()+logical.end,uint8_t(0));
        // Retarget the dispatcher and vtable references in the original image.
        if(!duplicate){
            for(size_t at=0;at+8<original.size();++at){
                if(original[at]==0xe8&&int64_t(at)+5+Read<int32_t>(original,at+1)==fn)Write<int32_t>(b,at+1,int32_t(dest-(at+5)));
                if(at%8==0&&Read<uintptr_t>(original,at)==base+fn)Write<uintptr_t>(b,at,base+dest);
            }
            for(auto& entry:records){
                if(entry.BeginAddress>=fn&&entry.EndAddress<=logical.end){entry.BeginAddress+=dest-fn;entry.EndAddress+=dest-fn;}
                auto uw=entry.UnwindInfoAddress;
                if((Read<uint8_t>(original,uw)>>3)&4){
                    auto chain=uw+4+((uint32_t(Read<uint8_t>(original,uw+2))+1)&~1u)*2;
                    auto parent=Read<IMAGE_RUNTIME_FUNCTION_ENTRY>(original,chain);
                    if(parent.BeginAddress>=fn&&parent.EndAddress<=logical.end){
                        parent.BeginAddress+=dest-fn;parent.EndAddress+=dest-fn;Write(b,chain,parent);
                    }
                }
            }
        }else{record.BeginAddress=dest;record.EndAddress=dest+length;records.push_back(record);break;}
    }
    auto newTable=next;next=(next+uint32_t(records.size()*12)+0xfff)&~0xfffu;b.resize(next);
    std::sort(records.begin(),records.end(),[](auto a,auto c){return a.BeginAddress<c.BeginAddress;});
    memcpy(b.data()+newTable,records.data(),records.size()*12);
    h.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION]={newTable,uint32_t(records.size()*12)};
    auto at=nt+4+sizeof(IMAGE_FILE_HEADER)+h.FileHeader.SizeOfOptionalHeader+h.FileHeader.NumberOfSections*sizeof(IMAGE_SECTION_HEADER);
    Check(at+sizeof(IMAGE_SECTION_HEADER)<=h.OptionalHeader.SizeOfHeaders,"room for moved fixture section");
    IMAGE_SECTION_HEADER s{};memcpy(s.Name,".moved",6);s.VirtualAddress=start;s.Misc.VirtualSize=next-start;
    s.Characteristics=IMAGE_SCN_MEM_READ|IMAGE_SCN_MEM_EXECUTE|IMAGE_SCN_CNT_CODE;Write(b,at,s);
    ++h.FileHeader.NumberOfSections;h.OptionalHeader.SizeOfImage=next;Write(b,nt,h);return b;
}
int wmain(int argc,wchar_t** argv){
    try{
        if(argc<2){std::cerr<<"Supply a local driver_vrlink.dll (never loaded/executed).\n";return 2;}
        uintptr_t base;auto b=Load(argv[1],base);eye::native::Layout l;std::string why;
        if(!Resolve(b,base,l,why)){
            eye::native::Image debug(b.data(),b.size(),base);debug.Parse();auto anchor=debug.AnchoredFunction("/sl/eyeTrackedGazePoint");
            std::cerr<<"receive anchor range "<<std::hex<<anchor.begin<<".."<<anchor.end<<std::dec<<"\n";
            throw std::runtime_error("discovery: "+why);
        }
        std::cout<<"{\"receive\":"<<l.receive<<",\"hmd_vtable\":"<<l.hmdVtable<<",\"present\":"<<l.present<<",\"this_offset\":"<<l.presentThis
                 <<",\"provider_hmd\":"<<l.providerHmd<<",\"owner\":"<<l.owner<<",\"session\":"<<l.session<<",\"gaze\":"<<l.gaze<<",\"timestamp\":"<<l.timestamp
                 <<",\"marker\":"<<l.marker<<",\"crop\":"<<l.crop<<",\"crop_size\":"<<l.cropSize<<"}\n";
        auto changed=b;auto nt=Read<uint32_t>(b,0x3c);
        Write<uint32_t>(changed,nt+4+offsetof(IMAGE_FILE_HEADER,TimeDateStamp),0x12345678);
        eye::native::Layout other;Check(Resolve(changed,base,other,why),"unrelated binary metadata must not invalidate layout");
        auto moved=MoveFunctions(b,base,l);if(!Resolve(moved,base,other,why))throw std::runtime_error("moved discovery: "+why);
        Check(other.receive!=l.receive&&other.present!=l.present&&other.gaze==l.gaze,"relocated functions discovered without old RVAs");
        auto ambiguous=MoveFunctions(b,base,l,true);Check(!Resolve(ambiguous,base,other,why),"ambiguous native handler rejected");
        eye::native::Image view(b.data(),b.size(),base);Check(view.Parse(),"parse for mutation");
        auto rx=view.Function(l.receive);auto stores=view.Unique(rx,{0x49,0x89,0x86,-1,-1,-1,-1,0xf3,0x0f,0x10,0x4e,8});
        changed=b;Write<uint32_t>(changed,stores+3,l.timestamp+8);Check(!Resolve(changed,base,other,why),"contradictory timestamp layout rejected");
        changed=b;Write<uint16_t>(changed,0,0);Check(!Resolve(changed,base,other,why),"invalid PE rejected");
        changed=b;Write<uint32_t>(changed,0x3c,0xfffffff0);Check(!Resolve(changed,base,other,why),"truncated NT headers rejected");
        std::cout<<"PASS: real DLL discovery, moved function RVAs, ambiguity, conflicting fields, malformed PE\n";return 0;
    }catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<"\n";return 1;}
}
