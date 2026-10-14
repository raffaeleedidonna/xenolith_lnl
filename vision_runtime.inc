/* Private runtime helpers. Transcript media is authoritative; .xve files are
 * disposable projected rows, keyed by source, weights, compiler and GPU/driver. */
enum { RT_IMAGE_HEADER=XE_IMAGE_RECORD_HEADER, RT_IMAGE_LIMIT=XE_IMAGE_RECORD_LIMIT };
#define RT_IMAGE_MEDIA_LIMIT (UINT64_C(256)*1024*1024)
#define RT_IMAGE_CACHE_LIMIT (UINT64_C(2)*1024*1024*1024)

static int runtime_visual_budget(int budget) {
    return budget==70||budget==140||budget==280||budget==560||budget==1120;
}

static int runtime_visual_payload(const conversation_block *block,xe_image *meta,
                                  int *offset,const uint8_t **media,size_t *size) {
    return block&&block->format==CONVERSATION_BLOCK_IMAGE &&
           xe_image_record_parse(block->data,(size_t)block->length,meta,offset,media,size);
}

static int runtime_visual_numeric(runtime *w) {
    if(!xe_engine_vision_fingerprint(w->engine)||
       (w->context!=65536&&w->context!=131072))return 0;
    if(!w->vision_numeric_ready) {
        if(!xe_engine_vision_numeric_fingerprint(w->engine,w->vision_numeric))return 0;
        w->vision_numeric_ready=1;
    }
    return 1;
}

static void runtime_visual_key(runtime *w,const xe_image *im,uint8_t key[32]) {
    format_sha256 hash;format_sha256_init(&hash);
    format_sha256_update(&hash,"xenolith-visual-cache-v1",24);
    format_sha256_update(&hash,im->identity,32);
    format_sha256_update(&hash,xe_engine_model_fingerprint(w->engine),32);
    format_sha256_update(&hash,xe_engine_vision_fingerprint(w->engine),32);
    format_sha256_update(&hash,w->vision_numeric,32);format_sha256_final(&hash,key);
}

static void runtime_visual_hex(const uint8_t key[32],char hex[65]) {
    static const char digits[]="0123456789abcdef";
    for(int i=0;i<32;i++){hex[2*i]=digits[key[i]>>4];hex[2*i+1]=digits[key[i]&15];}
    hex[64]=0;
}

static int runtime_visual_cache_path(runtime *w,const uint8_t key[32],char path[4096]) {
    if(!w->vision_cache_dir[0])return 0;
    if(mkdir(w->vision_cache_dir,0700)&&errno!=EEXIST)return 0;
    struct stat st;
    if(lstat(w->vision_cache_dir,&st)||!S_ISDIR(st.st_mode))return 0;
    char hex[65];runtime_visual_hex(key,hex);
    int n=snprintf(path,4096,"%s/%s.xve",w->vision_cache_dir,hex);
    return n>=0&&n<4096;
}

static int runtime_visual_cache_read(runtime *w,const xe_image *im,
                                      runtime_visual_asset *asset) {
    char path[4096];if(!runtime_visual_cache_path(w,asset->key,path))return 0;
    int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)return 0;
    FILE *f=fdopen(fd,"rb");if(!f){close(fd);return 0;}
    uint8_t h[160],hash[32];uint64_t size;size_t bytes=(size_t)im->rows*XE_VISUAL_WIDTH*4;
    int ok=format_file_size(f,&size)&&size==160+bytes&&format_read(f,h,160)&&
        !memcmp(h,"XEVEMB01",8)&&format_get_u32le(h+8)==(uint32_t)im->rows&&
        format_get_u32le(h+12)==(uint32_t)im->budget&&!memcmp(h+16,asset->key,32)&&
        !memcmp(h+112,im->encoded_hash,32)&&format_get_u64le(h+144)==bytes&&
        format_get_u64le(h+152)==format_crc64(0,h,152);
    if(ok)ok=format_read(f,asset->embeddings,bytes);
    fclose(f);
    if(ok) {
        format_sha256_bytes(asset->embeddings,bytes,hash);ok=!memcmp(hash,h+80,32);
        for(size_t i=0;ok&&i<bytes/4;i++)if(!isfinite(asset->embeddings[i]))ok=0;
    }
    if(ok) {
        xe_visual_identity(im,xe_engine_model_fingerprint(w->engine),
                           xe_engine_vision_fingerprint(w->engine),asset->embeddings,
                           XE_VISION_NUMERIC_REVISION,asset->identity);
        ok=!memcmp(asset->identity,h+48,32);
    }
    if(ok)(void)utimensat(AT_FDCWD,path,NULL,AT_SYMLINK_NOFOLLOW);
    return ok;
}

static int runtime_visual_cache_name(const char *name) {
    if(strlen(name)!=68||strcmp(name+64,".xve"))return 0;
    for(int i=0;i<64;i++)if(!((name[i]>='0'&&name[i]<='9')||(name[i]>='a'&&name[i]<='f')))return 0;
    return 1;
}

static void runtime_visual_cache_trim(runtime *w) {
    for(;;) {
        DIR *d=opendir(w->vision_cache_dir);if(!d)return;
        uint64_t total=0;time_t oldest=0;char victim[4096]={0};
        struct dirent *entry;
        while((entry=readdir(d))) {
            if(!runtime_visual_cache_name(entry->d_name))continue;
            char path[4096];int n=snprintf(path,sizeof path,"%s/%s",w->vision_cache_dir,entry->d_name);
            struct stat st;if(n<0||(size_t)n>=sizeof path||lstat(path,&st)||!S_ISREG(st.st_mode)||st.st_size<0)continue;
            total+=(uint64_t)st.st_size;
            if(!victim[0]||st.st_mtime<oldest) {
                oldest=st.st_mtime;memcpy(victim,path,(size_t)n+1);
            }
        }
        closedir(d);
        if(total<=RT_IMAGE_CACHE_LIMIT||!victim[0]||unlink(victim))return;
    }
}

static void runtime_visual_cache_write(runtime *w,const xe_image *im,
                                       const runtime_visual_asset *asset) {
    char path[4096];if(!runtime_visual_cache_path(w,asset->key,path))return;
    char temp[4096];int n=snprintf(temp,sizeof temp,"%s/.tmp-XXXXXX",w->vision_cache_dir);
    if(n<0||(size_t)n>=sizeof temp)return;
    int fd=mkstemp(temp);if(fd<0)return;
    FILE *f=fdopen(fd,"wb");if(!f){close(fd);unlink(temp);return;}
    uint8_t h[160]={0};size_t bytes=(size_t)im->rows*XE_VISUAL_WIDTH*4;
    memcpy(h,"XEVEMB01",8);format_put_u32le(h+8,im->rows);format_put_u32le(h+12,im->budget);
    memcpy(h+16,asset->key,32);memcpy(h+48,asset->identity,32);
    format_sha256_bytes(asset->embeddings,bytes,h+80);memcpy(h+112,im->encoded_hash,32);
    format_put_u64le(h+144,bytes);format_put_u64le(h+152,format_crc64(0,h,152));
    int ok=format_write(f,h,160)&&format_write(f,asset->embeddings,bytes)&&
           fflush(f)==0&&fsync(fileno(f))==0;
    if(fclose(f))ok=0;
    if(!ok||rename(temp,path)){unlink(temp);return;}
    runtime_visual_cache_trim(w);
}

static void runtime_visual_asset_free(runtime_visual_asset *a) {
    if(a){free(a->media);free(a->embeddings);free(a);}
}

static void runtime_visual_release(runtime *w) {
    for(size_t i=0;i<w->vision_asset_count;i++)runtime_visual_asset_free(w->vision_assets[i]);
    free(w->vision_assets);free(w->visual);
    w->vision_assets=NULL;w->visual=NULL;w->vision_asset_count=0;w->visual_count=0;
    w->vision_asset_capacity=0;w->visual_capacity=0;
}

static void runtime_visual_begin(runtime *w) {
    w->visual_count=0;
    if(++w->vision_epoch==0) {
        w->vision_epoch=1;
        for(size_t i=0;i<w->vision_asset_count;i++)w->vision_assets[i]->epoch=0;
    }
}

static void runtime_visual_prune(runtime *w) {
    size_t keep=0;
    for(size_t i=0;i<w->vision_asset_count;i++) {
        runtime_visual_asset *a=w->vision_assets[i];
        if(a->epoch==w->vision_epoch)w->vision_assets[keep++]=a;
        else runtime_visual_asset_free(a);
    }
    w->vision_asset_count=keep;
}

static runtime_status runtime_visual_asset_get(runtime *w,const xe_image *meta,
                                  const uint8_t *media,size_t size,const uint8_t *record,
                                  runtime_visual_asset **out) {
    uint8_t key[32];runtime_visual_key(w,meta,key);
    for(size_t i=0;i<w->vision_asset_count;i++) {
        runtime_visual_asset *a=w->vision_assets[i];
        if(!memcmp(a->key,key,32)&&a->media_size==size&&!memcmp(a->media,media,size)) {
            if(record&&!memcmp(record+208,w->vision_numeric,32)&&memcmp(record+112,a->identity,32))
                return runtime_fail(w,RUNTIME_INVALID_ARGUMENT,"image numerical replay mismatch");
            a->epoch=w->vision_epoch;*out=a;return RUNTIME_OK;
        }
    }
    uint8_t source_hash[32];format_sha256_bytes(media,size,source_hash);
    if(memcmp(source_hash,meta->encoded_hash,32))return runtime_fail(w,RUNTIME_INVALID_ARGUMENT,"stored image checksum mismatch");
    runtime_visual_asset *a=calloc(1,sizeof *a);if(!a)return runtime_fail(w,RUNTIME_NOMEM,"out of memory");
    memcpy(a->key,key,32);a->rows=meta->rows;a->budget=meta->budget;a->media_size=size;
    a->embeddings=malloc((size_t)a->rows*XE_VISUAL_WIDTH*4);a->media=malloc(size);
    if(!a->embeddings||!a->media){runtime_visual_asset_free(a);return runtime_fail(w,RUNTIME_NOMEM,"out of memory");}
    memcpy(a->media,media,size);
    if(!runtime_visual_cache_read(w,meta,a)) {
        xe_image im;char error[256];
        if(!xe_image_load_memory(&im,media,size,meta->budget,error,sizeof error)) {
            runtime_visual_asset_free(a);return runtime_fail(w,RUNTIME_INVALID_ARGUMENT,"invalid stored image");
        }
        int same=im.rows==meta->rows&&im.width==meta->width&&im.height==meta->height&&
            im.resized_width==meta->resized_width&&im.resized_height==meta->resized_height&&
            !memcmp(im.rgb_hash,meta->rgb_hash,32)&&!memcmp(im.resized_hash,meta->resized_hash,32)&&
            !memcmp(im.encoded_hash,meta->encoded_hash,32);
        if(!same) {
            xe_image_free(&im);runtime_visual_asset_free(a);
            return runtime_fail(w,RUNTIME_INVALID_ARGUMENT,"image preprocessing changed; rebuild required");
        }
        runtime_shadow_drop(w);
        int encoded=xe_engine_encode_image(w->engine,&im,a->embeddings,NULL);xe_image_free(&im);
        if(!encoded){runtime_visual_asset_free(a);return runtime_fail(w,RUNTIME_INVALID_ARGUMENT,"image encoding failed");}
        xe_visual_identity(meta,xe_engine_model_fingerprint(w->engine),xe_engine_vision_fingerprint(w->engine),
                           a->embeddings,XE_VISION_NUMERIC_REVISION,a->identity);
        runtime_visual_cache_write(w,meta,a);
    }
    if(record&&!memcmp(record+208,w->vision_numeric,32)&&memcmp(record+112,a->identity,32)) {
        runtime_visual_asset_free(a);return runtime_fail(w,RUNTIME_INVALID_ARGUMENT,"image numerical replay mismatch");
    }
    if(w->vision_asset_count==w->vision_asset_capacity) {
        size_t next=w->vision_asset_capacity?w->vision_asset_capacity*2:8;
        runtime_visual_asset **items=realloc(w->vision_assets,next*sizeof *items);
        if(!items){runtime_visual_asset_free(a);return runtime_fail(w,RUNTIME_NOMEM,"out of memory");}
        w->vision_assets=items;w->vision_asset_capacity=next;
    }
    a->epoch=w->vision_epoch;w->vision_assets[w->vision_asset_count++]=a;*out=a;return RUNTIME_OK;
}

static runtime_status runtime_visual_push(runtime *w,runtime_visual_asset *asset,int start) {
    uint64_t total=asset->media_size;
    for(size_t i=0;i<w->visual_count;i++)total+=w->visual[i].media_size;
    if(total>RT_IMAGE_MEDIA_LIMIT)return runtime_fail(w,RUNTIME_INVALID_ARGUMENT,"image media limit exceeded");
    if(w->visual_count==w->visual_capacity) {
        size_t next=w->visual_capacity?w->visual_capacity*2:8;
        xe_visual_span *spans=realloc(w->visual,next*sizeof *spans);
        if(!spans)return runtime_fail(w,RUNTIME_NOMEM,"out of memory");
        w->visual=spans;w->visual_capacity=next;
    }
    xe_visual_span *span=w->visual+w->visual_count++;
    *span=(xe_visual_span){.start=start,.rows=asset->rows,.embeddings=asset->embeddings,
        .media=asset->media,.media_size=asset->media_size,.budget=asset->budget};
    memcpy(span->identity,asset->identity,32);w->vision_touched=1;return RUNTIME_OK;
}

static runtime_status runtime_visual_collect(runtime *w,conversation *c) {
    runtime_visual_begin(w);
    uint64_t visible=conversation_visible_count(c),previous=0;
    for(uint64_t i=0;i<visible;i++) {
        uint64_t boundary=conversation_visible_boundary(c,i);
        const conversation_event *event=conversation_event_at(c,conversation_visible_index(c,i));
        int images=0;
        for(uint32_t j=0;j<event->block_count;j++) {
            const conversation_block *block=event->blocks+j;if(block->format!=CONVERSATION_BLOCK_IMAGE)continue;
            if(++images>1||!runtime_visual_numeric(w)||
               (event->role!=CONVERSATION_ROLE_USER&&event->type!=CONVERSATION_EVENT_TOOL_RESULT))
                return runtime_fail(w,RUNTIME_INVALID_ARGUMENT,"vision is not enabled or image layout is invalid");
            xe_image meta;int offset;const uint8_t *media;size_t size;
            if(!runtime_visual_payload(block,&meta,&offset,&media,&size)||
               boundary<previous||(uint64_t)offset+meta.rows>boundary-previous)
                return runtime_fail(w,RUNTIME_INVALID_ARGUMENT,"invalid image layout");
            const uint8_t *p=block->data;
            if(!xe_engine_vision_artifacts_match(w->engine, p+176, p+144))
                return runtime_fail(w,RUNTIME_INVALID_ARGUMENT,"image model mismatch; rebuild required");
            runtime_visual_asset *asset;runtime_status status=runtime_visual_asset_get(w,&meta,media,size,p,&asset);
            if(status!=RUNTIME_OK)return status;
            status=runtime_visual_push(w,asset,(int)previous+offset);if(status!=RUNTIME_OK)return status;
        }
        previous=boundary;
    }
    runtime_visual_prune(w);return RUNTIME_OK;
}

static xe_input runtime_visual_input(runtime *w,const int32_t *tokens,int count) {
    xe_input input={tokens,count,w->visual,0};
    while(input.visual_count<w->visual_count &&
          w->visual[input.visual_count].start+w->visual[input.visual_count].rows<=count)input.visual_count++;
    return input;
}

static int runtime_visual_boundary(const runtime *w,int position) {
    for(size_t i=0;i<w->visual_count;i++)
        if(position>w->visual[i].start && position<w->visual[i].start+w->visual[i].rows)return 0;
    return 1;
}

static int runtime_visual_common(runtime *w,xe_session *session,const xe_tokens *tokens) {
    if(!w->vision_touched)return xe_session_common(session,tokens);
    xe_input input=runtime_visual_input(w,tokens->v,tokens->len);
    return xe_session_common_input(session,&input);
}

static runtime_status runtime_visual_sync(runtime *w,xe_session *session,const xe_tokens *tokens,
                                         xe_sync_report *report) {
    if(!w->vision_touched){xe_session_sync_report(session,tokens,report);return RUNTIME_OK;}
    for(size_t i=0;i<w->visual_count;i++)if(tokens->len>w->visual[i].start &&
        tokens->len<w->visual[i].start+w->visual[i].rows)
        return runtime_fail(w,RUNTIME_INVALID_ARGUMENT,"partial visual prefix");
    xe_input input=runtime_visual_input(w,tokens->v,tokens->len);
    if(!xe_session_sync_input_report(session,&input,report))
        return runtime_fail(w,RUNTIME_INVALID_ARGUMENT,"invalid visual prefix");
    return RUNTIME_OK;
}

static int runtime_visual_target(runtime *w,int target) {
    for(size_t i=0;i<w->visual_count;i++) {
        const xe_visual_span *span=w->visual+i;
        if(target>span->start&&target<span->start+span->rows)return span->start+span->rows;
    }
    return target;
}

static runtime_status runtime_visual_media_budget(runtime *w,conversation *c,size_t extra) {
    uint64_t total=extra;
    for(uint64_t i=0;i<conversation_visible_count(c);i++) {
        const conversation_event *e=conversation_event_at(c,conversation_visible_index(c,i));
        for(uint32_t j=0;j<e->block_count;j++)if(e->blocks[j].format==CONVERSATION_BLOCK_IMAGE) {
            xe_image meta;int offset;const uint8_t *media;size_t size;
            if(!runtime_visual_payload(e->blocks+j,&meta,&offset,&media,&size))
                return runtime_fail(w,RUNTIME_INVALID_ARGUMENT,"invalid stored image");
            total+=size;
        }
    }
    if(total>RT_IMAGE_MEDIA_LIMIT)return runtime_fail(w,RUNTIME_INVALID_ARGUMENT,"image media limit exceeded");
    return RUNTIME_OK;
}

static runtime_status runtime_visual_prepare(runtime *w,const runtime_message *message,
                            const char *tool_name,uint32_t turn,uint64_t prefix,
                            profile_render *render,uint8_t **payload,size_t *length,
                            runtime_visual_asset **result,uint32_t *image_offset) {
    *payload=NULL;*length=0;
    if(!runtime_visual_numeric(w))return runtime_fail(w,RUNTIME_INVALID_ARGUMENT,"vision is not enabled");
    if(!message->image||!message->image_length||message->image_length>RT_IMAGE_LIMIT)
        return runtime_fail(w,RUNTIME_INVALID_ARGUMENT,"invalid image bytes");
    int budget=message->image_budget?message->image_budget:280;
    if(!runtime_visual_budget(budget))return runtime_fail(w,RUNTIME_INVALID_ARGUMENT,"invalid image budget");
    xe_image im;char error[256];
    if(!xe_image_load_memory(&im,message->image,message->image_length,budget,error,sizeof error))
        return runtime_fail(w,RUNTIME_INVALID_ARGUMENT,"invalid image");
    profile_status ps=tool_name?
        profile_render_tool_image(w->prof,tool_name,message->text?message->text:"",im.rows,render,image_offset):
        profile_render_user_image(w->prof,message->text?message->text:"",im.rows,turn,render,image_offset);
    runtime_status status=runtime_from_profile(w,ps);
    if(status==RUNTIME_OK)status=runtime_budget_check(w,prefix+render->token_count);
    if(status!=RUNTIME_OK){xe_image_free(&im);return status;}
    runtime_shadow_drop(w);w->vision_touched=1;
    runtime_visual_asset *asset;
    status=runtime_visual_asset_get(w,&im,message->image,message->image_length,NULL,&asset);
    if(status!=RUNTIME_OK){xe_image_free(&im);return status;}
    size_t bytes=RT_IMAGE_HEADER+message->image_length;uint8_t *p=calloc(1,bytes);
    if(!p){xe_image_free(&im);return runtime_fail(w,RUNTIME_NOMEM,"out of memory");}
    memcpy(p,"XEVIMG1\n",8);format_put_u32le(p+8,*image_offset);format_put_u32le(p+12,im.rows);
    format_put_u32le(p+16,im.budget);format_put_u32le(p+20,im.width);format_put_u32le(p+24,im.height);
    format_put_u32le(p+28,im.resized_width);format_put_u32le(p+32,im.resized_height);
    format_put_u32le(p+36,RT_IMAGE_HEADER);format_put_u64le(p+40,message->image_length);
    memcpy(p+48,im.encoded_hash,32);memcpy(p+80,im.rgb_hash,32);memcpy(p+112,asset->identity,32);
    memcpy(p+144,xe_engine_vision_fingerprint(w->engine),32);
    memcpy(p+176,xe_engine_model_fingerprint(w->engine),32);memcpy(p+208,w->vision_numeric,32);
    memcpy(p+240,im.resized_hash,32);memcpy(p+RT_IMAGE_HEADER,message->image,message->image_length);
    xe_image_free(&im);*payload=p;*length=bytes;if(result)*result=asset;return RUNTIME_OK;
}

static runtime_status runtime_visual_append(runtime *w,const runtime_message *message,runtime_marker *out) {
    conversation *c=w->current;
    const char *tool=NULL;
    if(message->kind==RUNTIME_MESSAGE_TOOL_RESULT) {
        if(runtime_turn(c)!=PROFILE_TURN_OPEN)return runtime_fail(w,RUNTIME_INVALID_ARGUMENT,"no open model turn for a tool result");
        tool=runtime_tool_name_of(c,message->call_id);
        if(!tool)return runtime_fail(w,RUNTIME_INVALID_ARGUMENT,"unknown tool call id");
    } else if(message->kind!=RUNTIME_MESSAGE_USER)
        return runtime_fail(w,RUNTIME_INVALID_ARGUMENT,"image role is not supported");
    runtime_status status=runtime_visual_media_budget(w,c,message->image_length);
    if(status!=RUNTIME_OK)return status;
    uint64_t tokens;conversation_tokens(c,&tokens);
    runtime_visual_begin(w);
    profile_render render;uint8_t *payload=NULL;size_t size=0;uint32_t offset;
    status=runtime_visual_prepare(w,message,tool,runtime_turn(c),tokens,&render,&payload,&size,NULL,&offset);
    if(status!=RUNTIME_OK){runtime_visual_prune(w);return status;}
    conversation_block blocks[2]={
        {CONVERSATION_BLOCK_TEXT,message->text?message->text:"",message->text?strlen(message->text):0},
        {CONVERSATION_BLOCK_IMAGE,payload,size}
    };
    if(tool)status=runtime_from_conversation(w,conversation_append_tool_result(c,message->call_id,
        message->tool_status?message->tool_status:CONVERSATION_TOOL_OK,blocks,2,render.render,
        render.render_length,render.tokens,render.token_count));
    else status=runtime_from_conversation(w,conversation_append_message(c,CONVERSATION_ROLE_USER,
        blocks,2,render.render,render.render_length,render.tokens,render.token_count));
    free(payload);
    runtime_marker marker=conversation_event_count(c)-1;
    conversation_settings settings;
    if(!conversation_get_settings(c,&settings))runtime_settings_defaults(&settings);
    if(status==RUNTIME_OK)status=runtime_project_mutation(w,c,
        settings.reasoning_effort!=CONVERSATION_REASONING_OFF,settings.reasoning_history);
    if(status==RUNTIME_OK)status=runtime_from_conversation(w,conversation_commit(c));
    if(status!=RUNTIME_OK)(void)conversation_rollback(c);
    runtime_visual_prune(w);
    if(status==RUNTIME_OK&&out)*out=marker;
    return status;
}

static int runtime_visual_present(const conversation *c) {
    for(uint64_t i=0;i<conversation_visible_count(c);i++) {
        const conversation_event *e=conversation_event_at(c,conversation_visible_index(c,i));
        for(uint32_t j=0;j<e->block_count;j++)if(e->blocks[j].format==CONVERSATION_BLOCK_IMAGE)return 1;
    }
    return 0;
}

runtime_status runtime_media(runtime *w,const uint8_t id[32],const uint8_t **bytes,size_t *size) {
    if(!w||!id||!bytes||!size)return RUNTIME_INVALID_ARGUMENT;
    if(!w->has_current)return runtime_fail(w,RUNTIME_SESSION_NOT_FOUND,"no open session");
    for(uint64_t i=0;i<conversation_event_count(w->current);i++) {
        const conversation_event *e=conversation_event_at(w->current,i);
        for(uint32_t j=0;j<e->block_count;j++)if(e->blocks[j].format==CONVERSATION_BLOCK_IMAGE) {
            xe_image meta;int offset;const uint8_t *media;size_t length;
            if(!runtime_visual_payload(e->blocks+j,&meta,&offset,&media,&length))
                return runtime_fail(w,RUNTIME_INVALID_ARGUMENT,"invalid stored image");
            if(!memcmp(meta.encoded_hash,id,32)) {
                uint8_t hash[32];format_sha256_bytes(media,length,hash);
                if(memcmp(hash,id,32))return runtime_fail(w,RUNTIME_INVALID_ARGUMENT,"stored image checksum mismatch");
                *bytes=media;*size=length;return RUNTIME_OK;
            }
        }
    }
    return runtime_fail(w,RUNTIME_INVALID_ARGUMENT,"image reference is unavailable");
}
