/* Optional protocol v2 image transport. Media IDs never name server paths. */
static int serve_image_hex(const char *text,size_t length,uint8_t out[32]) {
    if(!text||length!=64)return 0;
    for(size_t i=0;i<32;i++) {
        int value=0;
        for(int j=0;j<2;j++) {
            unsigned char c=(unsigned char)text[2*i+j];int n;
            if(c>='0'&&c<='9')n=c-'0';else if(c>='a'&&c<='f')n=c-'a'+10;
            else if(c>='A'&&c<='F')n=c-'A'+10;else return 0;
            value=value*16+n;
        }
        out[i]=(uint8_t)value;
    }
    return 1;
}

static int serve_image_digit(unsigned char c) {
    if(c>='A'&&c<='Z')return c-'A';
    if(c>='a'&&c<='z')return c-'a'+26;
    if(c>='0'&&c<='9')return c-'0'+52;
    if(c=='+')return 62;
    if(c=='/')return 63;
    return -1;
}

static uint8_t *serve_image_decode(const char *text,size_t length,size_t *size) {
    if(!text||!length||length%4||length>UINT64_C(64)*1024*1024)return NULL;
    size_t padding=(text[length-1]=='=')+(text[length-2]=='=');
    size_t bytes=length/4*3-padding;if(bytes<8||bytes>XE_IMAGE_RECORD_LIMIT)return NULL;
    uint8_t *out=malloc(bytes);if(!out)return NULL;
    size_t written=0;
    for(size_t i=0;i<length;i+=4) {
        int a=serve_image_digit(text[i]),b=serve_image_digit(text[i+1]);
        int c=serve_image_digit(text[i+2]),d=serve_image_digit(text[i+3]);
        int final=i+4==length;
        if(a<0||b<0||(!final&&(c<0||d<0)))goto bad;
        if(text[i+2]=='=') {
            if(!final||text[i+3]!='='||(b&15))goto bad;
            out[written++]=(uint8_t)(a<<2|b>>4);
        } else if(text[i+3]=='=') {
            if(!final||c<0||(c&3))goto bad;
            out[written++]=(uint8_t)(a<<2|b>>4);out[written++]=(uint8_t)(b<<4|c>>2);
        } else {
            if(c<0||d<0||written+3>bytes)goto bad;
            out[written++]=(uint8_t)(a<<2|b>>4);out[written++]=(uint8_t)(b<<4|c>>2);
            out[written++]=(uint8_t)(c<<6|d);
        }
    }
    if(written!=bytes)goto bad;
    *size=bytes;return out;
bad:
    free(out);return NULL;
}

static int serve_image_encode(json_writer *out,const uint8_t *bytes,size_t size) {
    static const char alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if(!json_raw(out,"\""))return 0;
    char chunk[4096];size_t used=0;
    for(size_t i=0;i<size;i+=3) {
        size_t n=size-i;if(n>3)n=3;
        uint32_t v=(uint32_t)bytes[i]<<16;if(n>1)v|=(uint32_t)bytes[i+1]<<8;if(n>2)v|=bytes[i+2];
        chunk[used++]=alphabet[v>>18];chunk[used++]=alphabet[(v>>12)&63];
        chunk[used++]=n>1?alphabet[(v>>6)&63]:'=';chunk[used++]=n>2?alphabet[v&63]:'=';
        if(used==sizeof chunk){if(!json_rawn(out,chunk,used))return 0;used=0;}
    }
    if(used&&!json_rawn(out,chunk,used))return 0;
    return json_raw(out,"\"");
}

static int serve_image_parse(serve *s,const json_value *message,runtime_message *out,
                             uint8_t **owned) {
    *owned=NULL;
    const json_value *image=json_member(message,"image");if(!image)return 1;
    if(image->type!=JSON_OBJECT||s->protocol!=2)return 0;
    const json_value *data=json_member(image,"data"),*id=json_member(image,"id");
    if(!!data==!!id)return 0;
    out->image_budget=280;
    const json_value *budget=json_member(image,"budget");
    if(budget) {
        if(budget->type!=JSON_NUMBER || !(budget->number==70||budget->number==140||budget->number==280||
            budget->number==560||budget->number==1120))return 0;
        out->image_budget=(int)budget->number;
    }
    if(data) {
        if(data->type!=JSON_STRING)return 0;
        *owned=serve_image_decode(data->text,data->text_length,&out->image_length);if(!*owned)return 0;
    } else {
        uint8_t hash[32];const uint8_t *media;size_t size;
        if(id->type!=JSON_STRING||!serve_image_hex(id->text,id->text_length,hash)||
           runtime_media(s->w,hash,&media,&size)!=RUNTIME_OK||size>XE_IMAGE_RECORD_LIMIT)return 0;
        *owned=malloc(size);if(!*owned)return 0;memcpy(*owned,media,size);out->image_length=size;
    }
    out->image=*owned;return 1;
}

static void serve_image_description(json_writer *out,const runtime_history_entry *entry) {
    uint8_t hash[32];char hex[65];format_sha256_bytes(entry->image,entry->image_length,hash);
    static const char digits[]="0123456789abcdef";
    for(int i=0;i<32;i++){hex[2*i]=digits[hash[i]>>4];hex[2*i+1]=digits[hash[i]&15];}hex[64]=0;
    json_raw(out,",\"image\":{\"id\":");json_string(out,hex,64);
    json_raw(out,",\"bytes\":");json_u64(out,entry->image_length);
    json_raw(out,",\"budget\":");json_i64(out,entry->image_budget);
    json_raw(out,",\"rows\":");json_i64(out,entry->image_rows);json_raw(out,"}");
}
