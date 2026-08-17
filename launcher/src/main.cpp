#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <dirent.h>
#include <sys/stat.h>
#include "esp_spiffs.h"
#include <vector>
#include <string>
#include <algorithm>

#define EMU_NES   0
#define EMU_ATARI 1
#define EMU_SMS   2
#include "video_out.h"
#include "ps2_input.h"
#include "font8x8.h"
#include "rtc_boot.h"

using std::string;
using std::vector;

#define SD_SCK_PIN 18
#define SD_MISO_PIN 19
#define SD_MOSI_PIN 23
#define SD_CS_PIN 21
#define SD_MOUNT "/sd"
#define W 384
#define H 240

static uint8_t *fb=0;
static uint8_t *rows[H];
static uint8_t io_banner[8][W];
static uint32_t pal[512];
struct BrowserEntry {
    string name;
    string path;
    bool is_dir;
};
static vector<BrowserEntry> games;
static vector<string> copy_marked;
static string current_path = "/sd";
static int selected=0, scroll_pos=0;
static bool import_mode=false;

static void make_palette() {
    for(int phase=0;phase<2;phase++) for(int i=0;i<256;i++) {
        int lum=i&15;
        uint8_t y=(uint8_t)(50 + lum*11); // safe monochrome composite levels
        pal[phase*256+i]=(uint32_t)y | ((uint32_t)y<<8) | ((uint32_t)y<<16) | ((uint32_t)y<<24);
    }
}
static void clear_screen(uint8_t c=0x00){ memset(fb,c,W*H); }
static void rect(int x,int y,int w,int h,uint8_t c){
    if(x<0){w+=x;x=0;} if(y<0){h+=y;y=0;} if(x+w>W)w=W-x; if(y+h>H)h=H-y;
    for(int yy=0;yy<h;yy++) memset(rows[y+yy]+x,c,w);
}
static void ch(int x,int y,char c,uint8_t fg,uint8_t bg){
    unsigned uc=(unsigned char)c; if(uc>127)uc='?';
    for(int yy=0;yy<8;yy++){
        uint8_t bits=font8x8[uc][yy];
        for(int xx=0;xx<8;xx++) rows[y+yy][x+xx]=(bits&(1<<xx))?fg:bg;
    }
}
static void text(int x,int y,const char*s,uint8_t fg=0x0F,uint8_t bg=0x00){
    while(*s && x<=W-8){ ch(x,y,*s++,fg,bg); x+=8; }
}
static bool video_ready=false;
static int io_hold_depth=0;
static bool io_hold_active=false;
static bool io_banner_ready=false;

static void build_io_banner(){
    if(io_banner_ready)return;
    io_banner_ready=true;
    memset(io_banner,0,sizeof(io_banner));
    const char *message="PLEASE WAIT - SD/SPIFFS I/O";
    int x=(W-(int)strlen(message)*8)/2;
    for(const char *s=message;*s;++s,x+=8){
        unsigned uc=(unsigned char)*s;if(uc>127)uc='?';
        for(int yy=0;yy<8;yy++){
            uint8_t bits=font8x8[uc][yy];
            for(int xx=0;xx<8;xx++)if(bits&(1<<xx))io_banner[yy][x+xx]=0x0F;
        }
    }
}

static void io_hold_begin(){
    if(io_hold_depth++!=0||!video_ready)return;
    build_io_banner();
    io_hold_active=video_io_hold_begin(&io_banner[0][0],116);
}

static void io_hold_end(){
    if(io_hold_depth<=0)return;
    if(--io_hold_depth==0&&io_hold_active){
        video_io_hold_end();
        io_hold_active=false;
    }
}

struct IOHold {
    IOHold(){io_hold_begin();}
    ~IOHold(){io_hold_end();}
};

static string ext(const string&s){ size_t p=s.find_last_of('.'); if(p==string::npos)return""; string e=s.substr(p+1); for(auto&c:e)c=tolower((unsigned char)c); return e; }
static bool wanted(const string&s){ string e=ext(s); return e=="atr"||e=="xex"; }
static bool is_copy_marked(const string& path){return std::find(copy_marked.begin(),copy_marked.end(),path)!=copy_marked.end();}
static void toggle_copy_mark(const string& path){auto i=std::find(copy_marked.begin(),copy_marked.end(),path);if(i==copy_marked.end())copy_marked.push_back(path);else copy_marked.erase(i);}
static string path_name(const string& path){size_t p=path.find_last_of('/');return p==string::npos?path:path.substr(p+1);}
static string file_label_with_extension(const string& prefix,const string& name,size_t max_len){
    string full=prefix+name;
    if(full.size()<=max_len)return full;
    size_t dot=name.find_last_of('.');
    string suffix=(dot!=string::npos)?name.substr(dot):"";
    size_t room=max_len>prefix.size()?max_len-prefix.size():0;
    if(suffix.empty()||room<=suffix.size())return full.substr(0,max_len);
    return prefix+name.substr(0,room-suffix.size())+suffix;
}
static bool mount_spiffs(){ esp_vfs_spiffs_conf_t c={.base_path="",.partition_label="spiffs",.max_files=5,.format_if_mount_failed=true}; esp_err_t e=esp_vfs_spiffs_register(&c); return e==ESP_OK||e==ESP_ERR_INVALID_STATE; }
static bool mount_sd(){ pinMode(SD_CS_PIN,OUTPUT);digitalWrite(SD_CS_PIN,HIGH);SPI.begin(SD_SCK_PIN,SD_MISO_PIN,SD_MOSI_PIN,SD_CS_PIN);delay(20);return SD.begin(SD_CS_PIN,SPI,10000000,SD_MOUNT,4,false)&&SD.cardType()!=CARD_NONE; }
static string join_path(const string& base,const string& name){ return base=="/" ? "/"+name : base+"/"+name; }
static string parent_path(const string& path){
    if(path=="/sd" || path.empty()) return "/sd";
    size_t p=path.find_last_of('/');
    if(p==string::npos || p<=3) return "/sd";
    return path.substr(0,p);
}
static void scan_games(){
    IOHold io;
    games.clear();
    DIR*d=opendir(current_path.c_str());
    if(!d){ current_path="/sd"; d=opendir(current_path.c_str()); }
    if(!d)return;

    // FAT/VFS does not have to return "." or "..". Add parent explicitly.
    if(current_path != "/sd")
        games.push_back({"..", parent_path(current_path), true});

    struct dirent*de;
    while((de=readdir(d))){
        string n=de->d_name;
        if(n=="." || n=="..") continue;
        string full=join_path(current_path,n);
        bool isdir=(de->d_type==DT_DIR);
        if(de->d_type==DT_UNKNOWN){ struct stat st; if(stat(full.c_str(),&st)==0) isdir=S_ISDIR(st.st_mode); }
        if(isdir || wanted(n)) games.push_back({n,full,isdir});
    }
    closedir(d);
    std::sort(games.begin(),games.end(),[](const BrowserEntry&a,const BrowserEntry&b){
        if(a.name=="..") return b.name!="..";
        if(b.name=="..") return false;
        if(a.is_dir!=b.is_dir) return a.is_dir>b.is_dir;
        return a.name<b.name;
    });
    selected=scroll_pos=0;
}
static void enter_directory(const string& path){ current_path=path; scan_games(); }
static bool go_parent(){ if(current_path=="/sd") return false; current_path=parent_path(current_path); scan_games(); return true; }
struct StoredGame {
    string name;
    string path;
    size_t size;
};

static vector<StoredGame> stored_games;
static bool delete_mode = false;
static bool format_mode = false;
static bool manual_delete_mode = false;
static uint32_t manual_delete_ready_at = 0;
static bool manual_del_armed = false;
static std::vector<bool> delete_marked;
static int delete_selected = 0;
static int delete_scroll = 0;
static string pending_source;
static string pending_dest;
static size_t pending_size = 0;
static size_t pending_existing_size = 0;
static size_t cached_spiffs_total = 0;
static size_t cached_spiffs_used = 0;
static const size_t SPIFFS_RESERVE = 32 * 1024;

static string safe_filename(const string& original)
{
    string out;
    out.reserve(original.size());

    for (unsigned char c : original) {
        if ((c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            c == '_' || c == '-' || c == '.' ||
            c == '!' || c == '(' || c == ')' ||
            c == '[' || c == ']' ||
            c == '+' || c == ',' || c == '\'' ||
            c == '#' || c == '&' || c == '@') {
            out += (char)c;
        } else {
            out += '_';
        }
    }

    // Keep the complete SPIFFS path under 30 characters.
    const size_t MAX_FULL_PATH = 30;
    const size_t FIXED_PATH_LEN = sizeof("/atari800/") - 1;
    const size_t MAX_NAME = MAX_FULL_PATH - FIXED_PATH_LEN;

    if (out.size() > MAX_NAME) {
        size_t dot = out.find_last_of('.');
        string extpart = (dot != string::npos && out.size() - dot <= 5) ? out.substr(dot) : "";
        size_t stem_len = MAX_NAME;
        if (extpart.size() < MAX_NAME) stem_len = MAX_NAME - extpart.size();
        out = out.substr(0, stem_len) + extpart;
    }

    if (out.empty()) out = "game.atr";
    return out;
}

static size_t file_size(const string& path)
{
    IOHold io;
    struct stat st;
    return stat(path.c_str(), &st) == 0 ? (size_t)st.st_size : 0;
}

static int remove_io(const string& path)
{
    IOHold io;
    return remove(path.c_str());
}

static bool refresh_spiffs_cache()
{
    IOHold io;
    size_t total = 0, used = 0;
    if (esp_spiffs_info("spiffs", &total, &used) != ESP_OK) return false;
    cached_spiffs_total = total;
    cached_spiffs_used = used;
    return true;
}

static size_t spiffs_free_bytes()
{
    // SPIFFS can reliably use only about 75% of its partition.  Treating the
    // whole value returned by esp_spiffs_info() as writable lets the launcher
    // reach a state where deletes succeed but subsequent writes still fail.
    const size_t usable = (cached_spiffs_total / 4) * 3;
    return usable > cached_spiffs_used
               ? usable - cached_spiffs_used
               : 0;
}

static size_t effective_free_for_pending()
{
    size_t freeb = spiffs_free_bytes();
    // If the same sanitized destination already exists, it will be replaced,
    // so its current size is reclaimable for this copy.
    freeb += pending_existing_size;
    return freeb;
}

static bool enough_space_for_pending()
{
    size_t freeb = effective_free_for_pending();
    return freeb >= pending_size + SPIFFS_RESERVE;
}

// Refresh the filesystem figures after every delete and immediately before
// opening the destination.  The actual write stays below the safe SPIFFS
// occupancy limit enforced by spiffs_free_bytes().
static bool prepare_spiffs_for_pending_copy()
{
    if (!refresh_spiffs_cache()) return false;
    return enough_space_for_pending();
}

static void scan_stored_games()
{
    IOHold io;
    stored_games.clear();
    DIR *d = opendir("/atari800");
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != nullptr) {
        if (de->d_type == DT_DIR) continue;
        string n = de->d_name;
        if (!wanted(n)) continue;
        string path = string("/atari800/") + n;
        // Do not offer the destination itself as a separate deletion choice;
        // it is automatically replaced if the selected SD game has same name.
        if (path == pending_dest) continue;
        stored_games.push_back({n, path, file_size(path)});
    }
    closedir(d);
    std::sort(stored_games.begin(), stored_games.end(), [](const StoredGame&a,const StoredGame&b){return a.name<b.name;});
    if (delete_selected >= (int)stored_games.size()) delete_selected = stored_games.empty()?0:(int)stored_games.size()-1;
    if (delete_selected < 0) delete_selected = 0;
}

static void open_manual_delete_dialog(){ IOHold io; pending_dest.clear(); pending_size=0; scan_stored_games(); delete_selected=delete_scroll=0; delete_marked.assign(stored_games.size(),false); manual_delete_mode=true; manual_delete_ready_at=millis()+250; manual_del_armed=false; }
static bool delete_marked_games(){ IOHold io; int n=0, failed=0; for(size_t i=0;i<stored_games.size()&&i<delete_marked.size();++i) if(delete_marked[i]){ Serial.printf("DELETE: %s\\n",stored_games[i].path.c_str()); if(remove_io(stored_games[i].path)==0)n++; else { failed++; Serial.printf("DELETE FAILED: %s errno=%d\\n",stored_games[i].path.c_str(),errno); } } bool info_ok=refresh_spiffs_cache(); Serial.printf("Deleted %d marked file(s), failed %d\\n",n,failed); scan_stored_games(); delete_marked.assign(stored_games.size(),false); delete_selected=delete_scroll=0; return failed==0&&info_ok; }

static bool copy_pending_game()
{
    IOHold io;
    struct stat dst_dir_st;
    if (stat("/atari800", &dst_dir_st) != 0)
        mkdir("/atari800", 0777);

    size_t fs_total=cached_spiffs_total, fs_used=cached_spiffs_used;
    size_t fs_free=spiffs_free_bytes();

    Serial.printf("COPY SRC: %s\n", pending_source.c_str());
    Serial.printf("COPY DST: %s\n", pending_dest.c_str());
    Serial.printf("SIZE: %u\n", (unsigned)pending_size);
    Serial.printf("SPIFFS TOTAL: %u USED: %u FREE: %u\n",
                  (unsigned)fs_total,(unsigned)fs_used,(unsigned)fs_free);

    FILE *in=fopen(pending_source.c_str(),"rb");
    if(!in){
        Serial.printf("SOURCE OPEN ERROR errno=%d\n",errno);
        return false;
    }

    // Remove same-name destination before GC so its pages can also be
    // reclaimed.  Never silently continue when the removal fails.
    if (pending_existing_size && remove_io(pending_dest) != 0) {
        Serial.printf("DESTINATION DELETE ERROR: %s errno=%d\n",
                      pending_dest.c_str(), errno);
        fclose(in);
        return false;
    }
    pending_existing_size = 0;

    if (!prepare_spiffs_for_pending_copy()) {
        Serial.println("SPIFFS has insufficient reclaimable space");
        fclose(in);
        return false;
    }

    FILE *out=fopen(pending_dest.c_str(),"wb");
    if(!out){
        Serial.printf("CREATE ERROR: %s errno=%d\n",pending_dest.c_str(),errno);
        fclose(in);
        return false;
    }

    uint8_t buf[1024];
    size_t total=0;
    bool ok=true;
    int saved_errno=0;

    for(;;){
        size_t n=fread(buf,1,sizeof(buf),in);
        if(!n){
            if(ferror(in)){
                saved_errno=errno;
                Serial.printf("READ ERROR at %u errno=%d\n",(unsigned)total,saved_errno);
                ok=false;
            }
            break;
        }

        size_t w=fwrite(buf,1,n,out);
        if(w!=n){
            saved_errno=errno;
            Serial.printf("WRITE FAILED at %u / %u, wanted=%u wrote=%u errno=%d\n",
                          (unsigned)total,(unsigned)pending_size,
                          (unsigned)n,(unsigned)w,saved_errno);
            ok=false;
            break;
        }
        total+=w;
    }

    if(fflush(out)!=0){
        saved_errno=errno;
        Serial.printf("FFLUSH FAILED errno=%d\n",saved_errno);
        ok=false;
    }

    fclose(out);
    fclose(in);

    if(!ok || total!=pending_size){
        Serial.printf("COPY FAILED total=%u expected=%u\n",
                      (unsigned)total,(unsigned)pending_size);
        remove_io(pending_dest);
        refresh_spiffs_cache();
        return false;
    }

    refresh_spiffs_cache();
    Serial.printf("Copied %u bytes: %s\n",(unsigned)total,pending_dest.c_str());
    return true;
}

static bool prepare_game(const string& source)
{
    IOHold io;
    pending_source = source;
    pending_size = file_size(pending_source);
    if (!pending_size) {
        Serial.printf("Cannot stat %s\n", pending_source.c_str());
        return false;
    }
    pending_dest = string("/atari800/") + safe_filename(path_name(source));
    pending_existing_size = file_size(pending_dest);
    return true;
}

static int start_copy_batch()
{
    IOHold io;
    vector<string> queue=copy_marked;
    if(queue.empty()&&!games.empty()&&!games[selected].is_dir)
        queue.push_back(games[selected].path);

    int copied=0;
    for(const string& source:queue){
        if(!prepare_game(source))return copied?copied:-1;
        if(!refresh_spiffs_cache() || !enough_space_for_pending()){
            scan_stored_games();
            delete_selected=0;
            delete_scroll=0;
            delete_mode=true;
            return copied;
        }
        if(!copy_pending_game())return copied?copied:-1;
        auto mark=std::find(copy_marked.begin(),copy_marked.end(),source);
        if(mark!=copy_marked.end())copy_marked.erase(mark);
        copied++;
    }
    return copied;
}
static void draw_ui(const char*status=0){
    video_sync();
    clear_screen(0x00); rect(28,20,328,200,0x01); rect(32,24,320,192,0x00);
    text(48,34,"ESP32 ATARI - SD LAUNCHER",0x0F,0x00);

    size_t fs_total=cached_spiffs_total, fs_used=cached_spiffs_used;
    size_t fs_free=fs_total>fs_used ? fs_total-fs_used : 0;
    char fsline[48];
    snprintf(fsline,sizeof(fsline),"SPIFFS FREE %u KB / %u KB",
             (unsigned)(fs_free/1024),(unsigned)(fs_total/1024));
    text(48,44,fsline,0x0E,0x00);

    text(48,54,"INS MARK  ENTER COPY/OPEN",0x0A,0x00);
    char markline[48];
    snprintf(markline,sizeof(markline),"DEL SPIFFS ESC BACK  MARKED %u",(unsigned)copy_marked.size());
    text(48,64,markline,0x0A,0x00);
    string shown_path=current_path; if(shown_path.size()>36) shown_path="..."+shown_path.substr(shown_path.size()-33);
    text(48,74,shown_path.c_str(),0x0B,0x00);
    if(status) text(48,198,status,0x0E,0x00);
    int visible=13; if(selected<scroll_pos)scroll_pos=selected; if(selected>=scroll_pos+visible)scroll_pos=selected-visible+1;
    for(int j=0;j<visible;j++){
        int i=scroll_pos+j; if(i>=(int)games.size())break;
        string n;
        if(games[i].is_dir){n=string("[DIR] ")+games[i].name;if(n.size()>34)n.resize(34);}
        else n=file_label_with_extension(is_copy_marked(games[i].path)?"[*] ":"[ ] ",games[i].name,34);
        int y=86+j*8; if(i==selected){rect(44,y,288,8,0x07);text(48,y,n.c_str(),0x00,0x07);} else text(48,y,n.c_str(),games[i].is_dir?0x0A:0x0F,0x00);
    }
}
static void draw_format_dialog(const char *status=0)
{
    video_sync();
    clear_screen(0x00);
    rect(54,54,276,112,0x01);
    rect(58,58,268,104,0x00);
    text(88,70,"FORMAT SPIFFS?",0x0E,0x00);
    text(72,90,"ALL GAMES WILL BE DELETED",0x0F,0x00);
    text(78,112,"ENTER = FORMAT",0x0A,0x00);
    text(78,124,"ESC   = CANCEL",0x0A,0x00);
    if(status) text(72,146,status,0x0E,0x00);
}

static bool format_spiffs_now()
{
    IOHold io;
    Serial.println("Formatting SPIFFS...");
    esp_vfs_spiffs_unregister("spiffs");

    esp_err_t e = esp_spiffs_format("spiffs");
    Serial.printf("esp_spiffs_format result=%d\n",(int)e);

    esp_vfs_spiffs_conf_t c = {
        .base_path = "",
        .partition_label = "spiffs",
        .max_files = 5,
        .format_if_mount_failed = true
    };
    esp_err_t m = esp_vfs_spiffs_register(&c);
    Serial.printf("SPIFFS remount result=%d\n",(int)m);

    size_t total=0, used=0;
    esp_err_t info=esp_spiffs_info("spiffs",&total,&used);
    if(info==ESP_OK) {
        cached_spiffs_total=total;
        cached_spiffs_used=used;
        Serial.printf("SPIFFS after format TOTAL=%u USED=%u FREE=%u\n",
                      (unsigned)total,(unsigned)used,
                      (unsigned)(total>used?total-used:0));
    }

    return e==ESP_OK && (m==ESP_OK || m==ESP_ERR_INVALID_STATE);
}

static void draw_manual_delete_dialog(const char*status=0){
 video_sync();
 clear_screen(0x00); rect(20,18,344,204,0x01); rect(24,22,336,196,0x00); text(40,30,"DELETE GAMES FROM SPIFFS",0x0E,0x00);
 size_t total=cached_spiffs_total,used=cached_spiffs_used; char line[48]; snprintf(line,sizeof(line),"FREE %uK / %uK",(unsigned)((total>used?total-used:0)/1024),(unsigned)(total/1024)); text(40,44,line,0x0F,0x00);
 text(40,58,"INS MARK/UNMARK  DEL DELETE",0x0A,0x00); text(40,70,"PGUP/PGDN MOVE  ESC BACK",0x0A,0x00); if(status)text(40,202,status,0x0E,0x00);
 int visible=13; if(delete_selected<delete_scroll)delete_scroll=delete_selected; if(delete_selected>=delete_scroll+visible)delete_scroll=delete_selected-visible+1;
 for(int j=0;j<visible;j++){int i=delete_scroll+j;if(i>=(int)stored_games.size())break;string n=stored_games[i].name;if(n.size()>23)n.resize(23);char mark=(i<(int)delete_marked.size()&&delete_marked[i])?'*':' ';snprintf(line,sizeof(line),"%c %-23s %3uK",mark,n.c_str(),(unsigned)((stored_games[i].size+1023)/1024));int y=84+j*8;if(i==delete_selected){rect(36,y,304,8,0x07);text(40,y,line,0x00,0x07);}else text(40,y,line,0x0F,0x00);} if(stored_games.empty())text(40,92,"NO ATR/XEX FILES",0x0C,0x00);
}

static void draw_delete_dialog(const char *status=0)
{
    video_sync();
    clear_screen(0x00);
    rect(20,18,344,204,0x01);
    rect(24,22,336,196,0x00);
    text(40,30,"NOT ENOUGH SPIFFS SPACE",0x0E,0x00);

    char line[48];
    size_t freeb = effective_free_for_pending();
    snprintf(line,sizeof(line),"NEED %uK+32K FREE %uK",
             (unsigned)((pending_size+1023)/1024),(unsigned)(freeb/1024));
    text(40,46,line,0x0F,0x00);
    text(40,60,"SELECT GAME TO DELETE",0x0A,0x00);
    text(40,72,"ENTER DELETE   ESC CANCEL",0x0A,0x00);
    if(status) text(40,202,status,0x0E,0x00);

    int visible=13;
    if(delete_selected<delete_scroll) delete_scroll=delete_selected;
    if(delete_selected>=delete_scroll+visible) delete_scroll=delete_selected-visible+1;
    for(int j=0;j<visible;j++){
        int i=delete_scroll+j;
        if(i>=(int)stored_games.size()) break;
        string n=stored_games[i].name;
        if(n.size()>25) n.resize(25);
        snprintf(line,sizeof(line),"%-25s %3uK",n.c_str(),(unsigned)((stored_games[i].size+1023)/1024));
        int y=88+j*8;
        if(i==delete_selected){rect(36,y,304,8,0x07);text(40,y,line,0x00,0x07);}
        else text(40,y,line,0x0F,0x00);
    }
    if(stored_games.empty()) text(40,96,"NO DELETABLE GAMES",0x0C,0x00);
}

static volatile uint8_t menu_key = 0;
static volatile bool f12_seen = false;

// Keep the PS/2 GPIO ISR on core 0. The PAL I2S ISR runs on the Arduino/app
// core and can otherwise starve the keyboard interrupt once video starts.
static void ps2_task(void *arg){
    ps2_init();
    for(;;){
        uint8_t h[10];
        int n=ps2_get_hid(h);
        if(n==10){
            uint8_t k=0;
            for(int i=4;i<10;i++) if(h[i]) { k=h[i]; break; }
            menu_key=k;
            if(k==0x45) f12_seen=true;
        }
        vTaskDelay(1);
    }
}

static bool f12_pressed(unsigned ms){
    uint32_t end=millis()+ms;
    while((int32_t)(millis()-end)<0){
        if(f12_seen) return true;
        delay(5);
    }
    return false;
}

static void process_keys(){
 static uint8_t last=0; static uint32_t repeat_at=0; static bool f2_latched=false; const uint32_t delay1=450,rate=90; const int page=12; uint8_t k=menu_key; uint32_t now=millis();
 if(k==0x3B&&!f2_latched&&!delete_mode&&!format_mode&&!manual_delete_mode){f2_latched=true;format_mode=true;last=0;draw_format_dialog();return;} if(k!=0x3B)f2_latched=false;
 if(format_mode){if(!k){last=0;return;}if(k==last)return;last=k;if(k==0x29){format_mode=false;draw_ui("FORMAT CANCELLED");}else if(k==0x28){draw_format_dialog("FORMATTING...");bool ok=format_spiffs_now();format_mode=false;draw_ui(ok?"SPIFFS FORMATTED":"FORMAT FAILED");}return;}
 if(manual_delete_mode){
    // Ignore the DEL that opened this modal for 250 ms. Do not wait for a
    // zero-key report: the PS/2 task may keep the last HID code latched.
    if((int32_t)(now-manual_delete_ready_at)<0){
        last=0;
        repeat_at=0;
        return;
    }
    if(!k){last=0;repeat_at=0;manual_del_armed=true;return;}
    // Any non-DEL key also proves we moved past the opening DEL.
    if(k!=0x4C) manual_del_armed=true;
    bool nav=k==0x52||k==0x51||k==0x4B||k==0x4E;if(nav){bool fire=false;if(k!=last){last=k;repeat_at=now+delay1;fire=true;}else if((int32_t)(now-repeat_at)>=0){repeat_at=now+rate;fire=true;}if(!fire)return;int n=stored_games.size();if(n){if(k==0x52)delete_selected--;else if(k==0x51)delete_selected++;else if(k==0x4B)delete_selected-=page;else delete_selected+=page;if(delete_selected<0)delete_selected=0;if(delete_selected>=n)delete_selected=n-1;}draw_manual_delete_dialog();return;}if(k==last)return;last=k;if(k==0x29){manual_delete_mode=false;draw_ui();}else if(k==0x49&&delete_selected<(int)delete_marked.size()){delete_marked[delete_selected]=!delete_marked[delete_selected];draw_manual_delete_dialog();}else if(k==0x4C && manual_del_armed){bool any=false;for(bool m:delete_marked)if(m){any=true;break;}if(any){bool ok=delete_marked_games();draw_manual_delete_dialog(ok?"MARKED FILES DELETED":"DELETE ERROR");}else draw_manual_delete_dialog("NOTHING MARKED");}return;}
 if(delete_mode){if(!k){last=0;repeat_at=0;return;}bool nav=k==0x52||k==0x51||k==0x4B||k==0x4E;if(nav){bool fire=false;if(k!=last){last=k;repeat_at=now+delay1;fire=true;}else if((int32_t)(now-repeat_at)>=0){repeat_at=now+rate;fire=true;}if(!fire)return;int n=stored_games.size();if(n){if(k==0x52)delete_selected--;else if(k==0x51)delete_selected++;else if(k==0x4B)delete_selected-=page;else delete_selected+=page;if(delete_selected<0)delete_selected=0;if(delete_selected>=n)delete_selected=n-1;}draw_delete_dialog();return;}if(k==last)return;last=k;if(k==0x29){delete_mode=false;draw_ui("DELETE CANCELLED");}else if(k==0x28&&!stored_games.empty()){string deleted_path=stored_games[delete_selected].path;if(remove_io(deleted_path)!=0){Serial.printf("DELETE FAILED: %s errno=%d\\n",deleted_path.c_str(),errno);draw_delete_dialog("DELETE ERROR");return;}if(!refresh_spiffs_cache()){draw_delete_dialog("SPIFFS INFO ERROR");return;}delay(50);scan_stored_games();if(enough_space_for_pending()){delete_mode=false;if(copy_pending_game()){auto mark=std::find(copy_marked.begin(),copy_marked.end(),pending_source);if(mark!=copy_marked.end())copy_marked.erase(mark);int more=copy_marked.empty()?0:start_copy_batch();if(delete_mode)draw_delete_dialog("MORE SPACE NEEDED");else if(more<0)draw_ui("COPY/WRITE ERROR");else draw_ui("COPY DONE - NO RESTART");}else draw_ui("COPY/WRITE ERROR");}else draw_delete_dialog("MORE SPACE NEEDED");}return;}
 if(!k){last=0;repeat_at=0;return;} if(k==0x4C&&k!=last){last=k;last=0;repeat_at=0;open_manual_delete_dialog();draw_manual_delete_dialog();return;} if(k==0x49&&k!=last){last=k;if(!games.empty()&&!games[selected].is_dir)toggle_copy_mark(games[selected].path);draw_ui();return;}
 bool nav=k==0x52||k==0x51||k==0x4B||k==0x4E;if(nav){bool fire=false;if(k!=last){last=k;repeat_at=now+delay1;fire=true;}else if((int32_t)(now-repeat_at)>=0){repeat_at=now+rate;fire=true;}if(!fire)return;int n=games.size();if(n){if(k==0x52)selected--;else if(k==0x51)selected++;else if(k==0x4B)selected-=page;else selected+=page;if(selected<0)selected=0;if(selected>=n)selected=n-1;}draw_ui();return;}
 if(k==last)return;last=k;if(k==0x29){if(go_parent())draw_ui("PARENT");else{draw_ui("STARTING ATARI...");delay(100);boot_atari_once();}}else if(k==0x28&&!games.empty()){if(games[selected].is_dir&&copy_marked.empty()){enter_directory(games[selected].path);draw_ui("FOLDER");}else{int copied=start_copy_batch();if(delete_mode)draw_delete_dialog();else if(copied<0)draw_ui("COPY/WRITE ERROR");else if(copied==0)draw_ui("NOTHING COPIED");else draw_ui("COPY DONE - NO RESTART");}}
}

void setup(){
    Serial.begin(115200);
    xTaskCreatePinnedToCore(ps2_task,"ps2_task",2048,NULL,2,NULL,0);
    delay(50);
    if(!f12_pressed(1200)){ boot_atari_once(); return; }
    if(!mount_spiffs()){ boot_atari_once(); return; }
    if(!refresh_spiffs_cache()){ boot_atari_once(); return; }
    if(!mount_sd()){ boot_atari_once(); return; }
    { DIR *d=opendir("/sd/atari800"); if(d){ closedir(d); current_path="/sd/atari800"; } else current_path="/sd"; }
    scan_games();
    fb=(uint8_t*)heap_caps_malloc(W*H,MALLOC_CAP_8BIT); if(!fb){boot_atari_once();return;}
    for(int y=0;y<H;y++)rows[y]=fb+y*W;
    make_palette(); clear_screen(); _lines=rows;
    video_init(4,EMU_ATARI,pal,0); // PAL composite, same Atari video path
    build_io_banner();
    if(video_io_hold_begin(&io_banner[0][0],116))video_io_hold_end();
    video_ready=true;
    import_mode=true; draw_ui(games.empty()?"NO ATR/XEX FILES":"SD READY");
}
void loop(){ if(import_mode)process_keys(); delay(2); }
