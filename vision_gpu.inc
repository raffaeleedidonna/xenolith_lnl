/* Included by xenolith.c: uses the existing Level Zero owner and allocator. */
static ze_kernel_handle_t xe_vision_kernel(xe_engine *e, int index) {
    static const char *names[8] = {
        "xe_vision_dense", "xe_vision_norm", "xe_vision_head", "xe_vision_attention",
        "xe_vision_add", "xe_vision_geglu", "xe_vision_pool", "xe_prefill_attn_online_b8_swa_visual"
    };
    if (!e->gpu.vision[index]) {
        ze_kernel_desc_t desc = { .stype=ZE_STRUCTURE_TYPE_KERNEL_DESC, .pKernelName=names[index] };
        xe_ze_check("zeKernelCreate vision", zeKernelCreate(e->gpu.module,&desc,&e->gpu.vision[index]));
        xe_ze_check("zeKernelSetGroupSize vision", zeKernelSetGroupSize(e->gpu.vision[index],128,1,1));
    }
    return e->gpu.vision[index];
}

static void xe_vision_launch(xe_engine *e, ze_kernel_handle_t kernel, int gx, int gy) {
    ze_group_count_t groups={(uint32_t)gx,(uint32_t)gy,1};
    xe_ze_check("zeCommandListAppendLaunchKernel vision",
                zeCommandListAppendLaunchKernel(e->gpu.commands,kernel,&groups,NULL,0,NULL));
    /* Production must bound queued GPU work without relying on dump I/O. */
    xe_ze_check("zeCommandListHostSynchronize vision dispatch",
                zeCommandListHostSynchronize(e->gpu.commands,UINT64_MAX));
}

static const void *xe_vision_weight(xe_engine *e,const char *name,int type) {
    const xe_vision_tensor *t=xe_vision_tensor_get(&e->vision_model,name);
    if(!t||t->type!=(uint32_t)type)xe_fatal("vision: missing or invalid tensor %s",name);
    return t->data;
}

int xe_engine_enable_vision(xe_engine *e,const char *path,char *error,size_t cap) {
    if(!e||e->vision_weights){if(error&&cap)snprintf(error,cap,"vision already open or engine missing");return 0;}
    xe_worker_pool_init(e);
    xe_require_owner(e);
    static const uint8_t expected_model[32]={0xa7,0xc5,0xbc,0x71,0x5f,0x5f,0xf8,0xe9,0x9a,0x3e,0x89,0x01,0xce,0x7d,0x2b,0x42,0xb4,0x02,0xc6,0x69,0xbf,0x24,0xf7,0xc5,0x25,0x07,0x47,0x63,0x3d,0x0f,0x58,0x91};
    if(memcmp(xe_engine_model_fingerprint(e),expected_model,32)) {
        if(error&&cap)snprintf(error,cap,"vision model checksum mismatch; see vision/manifest.json");
        return 0;
    }
    xe_vision_model model;
    if(!xe_vision_model_open(&model,path,error,cap))return 0;
    if(!e->gpu.context)xe_gpu_init(e);
    size_t bytes=model.mapping_size-model.data_offset;
    e->vision_weights=xe_alloc(e,bytes,XE_MEM_SHARED);
    memcpy(e->vision_weights,(const uint8_t *)model.mapping+model.data_offset,bytes);
    for(size_t i=0;i<model.tensor_count;i++)model.tensors[i].data=(const uint8_t *)e->vision_weights+model.tensors[i].offset;
    munmap(model.mapping,model.mapping_size);model.mapping=NULL;
    e->vision_model=model;
    return 1;
}

static void xe_vision_dense(xe_engine *e,const float *x,const char *weight,
                            float *out,int rows,int width,int columns) {
    ze_kernel_handle_t k=xe_vision_kernel(e,0);
    xe_gpu_pointer_arg(k,0,x);xe_gpu_pointer_arg(k,1,xe_vision_weight(e,weight,1));xe_gpu_pointer_arg(k,2,out);
    xe_gpu_int_arg(k,3,rows);xe_gpu_int_arg(k,4,width);xe_gpu_int_arg(k,5,columns);
    xe_vision_launch(e,k,(rows+15)/16,(columns+15)/16);
}

static void xe_vision_norm(xe_engine *e,const float *x,const char *weight,
                           float *out,int rows,int width) {
    ze_kernel_handle_t k=xe_vision_kernel(e,1);
    xe_gpu_pointer_arg(k,0,x);xe_gpu_pointer_arg(k,1,weight?xe_vision_weight(e,weight,0):NULL);xe_gpu_pointer_arg(k,2,out);
    xe_gpu_int_arg(k,3,rows);xe_gpu_int_arg(k,4,width);xe_vision_launch(e,k,rows,1);
}

static void xe_vision_head(xe_engine *e,float *x,const char *weight,int rows,int columns,int rotary) {
    ze_kernel_handle_t k=xe_vision_kernel(e,2);
    xe_gpu_pointer_arg(k,0,x);xe_gpu_pointer_arg(k,1,weight?xe_vision_weight(e,weight,0):NULL);
    xe_gpu_int_arg(k,2,rows);xe_gpu_int_arg(k,3,columns);xe_gpu_int_arg(k,4,rotary);
    xe_vision_launch(e,k,rows*16,1);
}

static void xe_vision_binary(xe_engine *e,int index,float *x,const float *y,int elements) {
    ze_kernel_handle_t k=xe_vision_kernel(e,index);
    xe_gpu_pointer_arg(k,0,x);xe_gpu_pointer_arg(k,1,y);xe_gpu_int_arg(k,2,elements);
    xe_vision_launch(e,k,(elements+127)/128,1);
}

static void xe_vision_wait(xe_engine *e) {
    xe_ze_check("zeCommandListHostSynchronize vision",zeCommandListHostSynchronize(e->gpu.commands,UINT64_MAX));
}

static void xe_vision_dump(xe_engine *e,const char *directory,const char *name,
                           const float *x,size_t elements) {
    if(!directory)return;
    xe_vision_wait(e);
    char path[4096];int n=snprintf(path,sizeof path,"%s/%s.f32",directory,name);
    if(n<0||(size_t)n>=sizeof path)xe_fatal("vision dump path too long");
    FILE *f=fopen(path,"wb");if(!f)xe_fatal("cannot open vision dump %s",path);
    int ok=format_write(f,x,elements*sizeof(float));if(fclose(f))ok=0;
    if(!ok)xe_fatal("cannot write vision dump %s",path);
}

int xe_engine_encode_image(xe_engine *e,const xe_image *im,float *output,const char *dump) {
    if(!e||!im||!output||!e->vision_weights||!im->resized||im->rows<1||im->rows>1120)return 0;
    xe_require_owner(e);
    int cols=im->resized_width/16,rows=im->resized_height/16,n=rows*cols;
    if(cols%3||rows%3||n!=im->rows*9)return 0;
    size_t wide=(size_t)n*1152,ff=(size_t)n*4304;
    float *patch=xe_alloc(e,(size_t)n*768*sizeof(float),XE_MEM_SHARED);
    for(int py=0;py<rows;py++)for(int px=0;px<cols;px++)
        for(int c=0;c<3;c++)for(int y=0;y<16;y++)for(int x=0;x<16;x++) {
            unsigned char v=im->resized[((size_t)(py*16+y)*im->resized_width+px*16+x)*3+c];
            patch[((size_t)py*cols+px)*768+c*256+y*16+x]=((float)v/255)*2-1;
        }
    float *hidden=xe_alloc(e,wide*4,XE_MEM_SHARED),*norm=xe_alloc(e,wide*4,XE_MEM_SHARED);
    float *q=xe_alloc(e,wide*4,XE_MEM_SHARED),*k=xe_alloc(e,wide*4,XE_MEM_SHARED),*v=xe_alloc(e,wide*4,XE_MEM_SHARED);
    float *attention=xe_alloc(e,wide*4,XE_MEM_SHARED),*projected=xe_alloc(e,wide*4,XE_MEM_SHARED);
    float *gate=xe_alloc(e,ff*4,XE_MEM_SHARED),*up=xe_alloc(e,ff*4,XE_MEM_SHARED);
    xe_vision_dense(e,patch,"v.patch_embd.weight",hidden,n,768,1152);xe_vision_wait(e);
    xe_free(e,patch,XE_MEM_SHARED);
    const float *pos=xe_vision_weight(e,"v.position_embd.weight",0);
    for(int row=0;row<n;row++)for(int d=0;d<1152;d++)
        hidden[(size_t)row*1152+d]+=pos[(size_t)(row%cols)*1152+d]+pos[(size_t)(10240+row/cols)*1152+d];
    xe_vision_dump(e,dump,"pos_embd",hidden,wide);
    for(int layer=0;layer<27;layer++) {
        char name[128];
#define VW(suffix) (snprintf(name,sizeof name,"v.blk.%d.%s",layer,suffix),name)
        xe_vision_norm(e,hidden,VW("ln1.weight"),norm,n,1152);
        xe_vision_dense(e,norm,VW("attn_q.weight"),q,n,1152,1152);
        xe_vision_dense(e,norm,VW("attn_k.weight"),k,n,1152,1152);
        xe_vision_dense(e,norm,VW("attn_v.weight"),v,n,1152,1152);
        xe_vision_head(e,q,VW("attn_q_norm.weight"),n,cols,1);
        xe_vision_head(e,k,VW("attn_k_norm.weight"),n,cols,1);
        xe_vision_head(e,v,NULL,n,cols,0);
        ze_kernel_handle_t att=xe_vision_kernel(e,3);
        xe_gpu_pointer_arg(att,0,q);xe_gpu_pointer_arg(att,1,k);xe_gpu_pointer_arg(att,2,v);xe_gpu_pointer_arg(att,3,attention);
        xe_gpu_int_arg(att,4,n);
        /* Tile queries only: every query still reads every image key/value.
         * A monolithic 10080-patch dispatch can trigger WSL device loss. */
        for(int first=0;first<n;first+=128) {
            int queries=n-first;if(queries>128)queries=128;
            xe_gpu_int_arg(att,5,first*16);xe_vision_launch(e,att,(queries*16+7)/8,1);
        }
        xe_vision_dense(e,attention,VW("attn_out.weight"),projected,n,1152,1152);
        xe_vision_norm(e,projected,VW("attn_post_norm.weight"),norm,n,1152);
        xe_vision_binary(e,4,hidden,norm,(int)wide);
        xe_vision_norm(e,hidden,VW("ln2.weight"),norm,n,1152);
        xe_vision_dense(e,norm,VW("ffn_gate.weight"),gate,n,1152,4304);
        xe_vision_dense(e,norm,VW("ffn_up.weight"),up,n,1152,4304);
        xe_vision_binary(e,5,gate,up,(int)ff);
        xe_vision_dense(e,gate,VW("ffn_down.weight"),projected,n,4304,1152);
        xe_vision_norm(e,projected,VW("ffn_post_norm.weight"),norm,n,1152);
        xe_vision_binary(e,4,hidden,norm,(int)wide);
        snprintf(name,sizeof name,"layer_out-%d",layer);xe_vision_dump(e,dump,name,hidden,wide);
#undef VW
    }
    ze_kernel_handle_t pool=xe_vision_kernel(e,6);
    xe_gpu_pointer_arg(pool,0,hidden);xe_gpu_pointer_arg(pool,1,xe_vision_weight(e,"v.std_bias",0));
    xe_gpu_pointer_arg(pool,2,xe_vision_weight(e,"v.std_scale",0));xe_gpu_pointer_arg(pool,3,norm);
    xe_gpu_int_arg(pool,4,cols);xe_gpu_int_arg(pool,5,im->rows);
    xe_vision_launch(e,pool,(im->rows*1152+127)/128,1);
    xe_vision_dump(e,dump,"std_scaled",norm,(size_t)im->rows*1152);
    xe_vision_norm(e,norm,NULL,projected,im->rows,1152);
    float *result=xe_alloc(e,(size_t)im->rows*2816*4,XE_MEM_SHARED);
    xe_vision_dense(e,projected,"mm.input_projection.weight",result,im->rows,1152,2816);
    xe_vision_wait(e);
    for(size_t i=0;i<(size_t)im->rows*2816;i++)if(!isfinite(result[i]))xe_fatal("vision: nonfinite projected embedding");
    memcpy(output,result,(size_t)im->rows*2816*4);
    xe_vision_dump(e,dump,"embedding",result,(size_t)im->rows*2816);
    xe_free(e,result,XE_MEM_SHARED);xe_free(e,up,XE_MEM_SHARED);xe_free(e,gate,XE_MEM_SHARED);
    xe_free(e,projected,XE_MEM_SHARED);xe_free(e,attention,XE_MEM_SHARED);
    xe_free(e,v,XE_MEM_SHARED);xe_free(e,k,XE_MEM_SHARED);xe_free(e,q,XE_MEM_SHARED);
    xe_free(e,norm,XE_MEM_SHARED);xe_free(e,hidden,XE_MEM_SHARED);
    return 1;
}

static int xe_session_prefill_reserve(xe_session *s,int rows) {
    if(rows<=s->prefill_capacity)return 1;
    if(rows>XE_VISUAL_MAX||s->cow||s->prefill_pending)return 0;
    xe_prefill_workspace w;
    size_t bytes=xe_prefill_workspace_layout_rows(&w,NULL,rows);
    void *next=xe_alloc(s->engine,bytes,XE_MEM_SHARED);
    xe_free(s->engine,s->prefill_workspace,XE_MEM_SHARED);
    s->prefill_workspace=next;s->prefill_workspace_size=bytes;s->prefill_capacity=rows;
    return 1;
}

const uint8_t *xe_engine_model_fingerprint(xe_engine *e) {
    if(!e || !e->map || !e->map_len)return NULL;
    xe_worker_pool_init(e);xe_require_owner(e);
    if(!e->model_content_hash_ready) {
        format_sha256_bytes(e->map,e->map_len,e->model_content_hash);
        e->model_content_hash_ready=1;
    }
    return e->model_content_hash;
}

const uint8_t *xe_engine_vision_fingerprint(const xe_engine *e) {
    return e && e->vision_weights ? e->vision_model.sha256 : NULL;
}

int xe_engine_vision_numeric_fingerprint(xe_engine *e,uint8_t out[32]) {
    if(!e||!e->gpu.context||!out)return 0;
    xe_require_owner(e);
    if(e->vision_numeric_hash_ready){memcpy(out,e->vision_numeric_hash,32);return 1;}
    ze_device_properties_t device={.stype=ZE_STRUCTURE_TYPE_DEVICE_PROPERTIES};
    ze_driver_properties_t driver={.stype=ZE_STRUCTURE_TYPE_DRIVER_PROPERTIES};
    if(zeDeviceGetProperties(e->gpu.device,&device)!=ZE_RESULT_SUCCESS ||
       zeDriverGetProperties(e->gpu.driver,&driver)!=ZE_RESULT_SUCCESS)return 0;
    size_t native_size=0;
    if(zeModuleGetNativeBinary(e->gpu.module,&native_size,NULL)!=ZE_RESULT_SUCCESS||!native_size)return 0;
    uint8_t *native=malloc(native_size);if(!native)return 0;
    if(zeModuleGetNativeBinary(e->gpu.module,&native_size,native)!=ZE_RESULT_SUCCESS){free(native);return 0;}
    format_sha256 hash;format_sha256_init(&hash);
    const char *domain="xenolith-visual-numeric-v2";
    format_sha256_update(&hash,domain,strlen(domain));
    format_sha256_update(&hash,XE_VISION_NUMERIC_REVISION,strlen(XE_VISION_NUMERIC_REVISION));
    format_sha256_update(&hash,XE_NUMERIC_SOURCE_HASH,strlen(XE_NUMERIC_SOURCE_HASH));
    format_sha256_update(&hash,XE_NUMERIC_CFLAGS_HASH,strlen(XE_NUMERIC_CFLAGS_HASH));
    format_sha256_update(&hash,__VERSION__,strlen(__VERSION__));
    format_sha256_update(&hash,_binary_xenolith_gpu_spv_start,
                         (size_t)(_binary_xenolith_gpu_spv_end-_binary_xenolith_gpu_spv_start));
    format_sha256_update(&hash,device.uuid.id,sizeof device.uuid.id);
    /* Intel's driver UUID contains an initialization timestamp. It names a
     * driver instance, not stable numerical behavior across restarts. Bind
     * the actual compiled native module instead, including compiler changes. */
    format_sha256_update(&hash,native,native_size);free(native);
    uint8_t bytes[4];format_put_u32le(bytes,driver.driverVersion);
    format_sha256_update(&hash,bytes,4);format_sha256_final(&hash,e->vision_numeric_hash);
    e->vision_numeric_hash_ready=1;memcpy(out,e->vision_numeric_hash,32);return 1;
}

int xe_session_set_text_prefill(xe_session *s,int rows) {
    if(!s||(rows!=512&&rows!=1024)||s->n_tokens||s->prefill_pending||s->cow)return 0;
    xe_require_owner(s->engine);
    if(!xe_session_prefill_reserve(s,rows))return 0;
    s->text_prefill_rows=rows;return 1;
}

static xe_visual_context *xe_visual_context_new(const xe_input *in) {
    xe_visual_context *v=calloc(1,sizeof *v);
    if (!v) return NULL;
    if (!xe_input_copy(&v->owned,in)) { free(v);return NULL; }
    v->references=1;return v;
}

static int xe_session_common_input_validated(const xe_session *s,const xe_input *in) {
    int common=0,limit=in->count<s->n_tokens?in->count:s->n_tokens;
    while(common<limit&&in->tokens[common]==s->tokens[common])common++;
    const xe_input *old=s->visual_context?&s->visual_context->owned.input:NULL;
    size_t i=0,j=0,n=old?old->visual_count:0;
    while(i<n||j<in->visual_count) {
        const xe_visual_span *a=i<n?old->visual+i:NULL,*b=j<in->visual_count?in->visual+j:NULL;
        int first=a&&b?(a->start<b->start?a->start:b->start):a?a->start:b->start;
        if(first>=common)break;
        if(!a||!b||a->start!=b->start||a->rows!=b->rows||a->budget!=b->budget||
           a->media_size!=b->media_size||memcmp(a->identity,b->identity,32)||
           memcmp(a->embeddings,b->embeddings,(size_t)a->rows*XE_VISUAL_WIDTH*4)||
           (a->media_size&&memcmp(a->media,b->media,a->media_size)))return first;
        if(common<a->start+a->rows)return a->start;
        i++;j++;
    }
    return common;
}

int xe_session_common_input(const xe_session *s,const xe_input *in) {
    if(!s||!xe_input_valid(in,s->engine->context))return -1;
    return xe_session_common_input_validated(s,in);
}

int xe_session_has_visual(const xe_session *s) {
    return s && s->visual_context!=NULL;
}

int xe_session_sync_input_report(xe_session *s,const xe_input *in,xe_sync_report *report) {
    if(!s||s->cow||s->prefill_pending||!xe_input_layout_valid(in,s->engine->context))return 0;
    xe_require_owner(s->engine);
    int previous=s->n_tokens;
    if(report)memset(report,0,sizeof *report);
    for(int i=0;i<in->count;i++)if(in->tokens[i]<0||in->tokens[i]>=XE_VOCAB)return 0;
    for(size_t i=0;i<in->visual_count;i++)
        for(int j=0;j<in->visual[i].rows;j++)
            if(in->tokens[in->visual[i].start+j]!=xe_token_id(s->engine,"<|image|>"))return 0;
    if(!in->visual_count) {
        int restarted=s->visual_context!=NULL;
        if(restarted)xe_session_reset(s);
        xe_tokens prefix={(int32_t *)in->tokens,in->count,in->count};
        xe_session_sync_report_impl(s,&prefix,report);
        if(report&&restarted)report->restarted=1;
        return 1;
    }
    if(s->visual_context && s->n_tokens>=s->visual_prompt_end && in->count>=s->visual_prompt_end &&
       in->visual_count==s->visual_context->owned.input.visual_count &&
       xe_session_common_input_validated(s,in)==s->n_tokens) {
        /* The old owned rows are finite. Equal byte content proves the new
         * rows are unchanged, without rehashing all pixels on every token. */
        xe_tokens prefix={(int32_t *)in->tokens,in->count,in->count};
        xe_session_sync_report_impl(s,&prefix,report);return 1;
    }
    int append=xe_session_common_input_validated(s,in)==s->n_tokens && in->count>s->n_tokens;
    xe_visual_context *next=xe_visual_context_new(in);
    if(!next)return 0;
    int capacity=s->text_prefill_rows;
    for(size_t i=0;i<in->visual_count;i++)if(in->visual[i].rows>capacity)capacity=in->visual[i].rows;
    if(!xe_session_prefill_reserve(s,capacity)){xe_visual_context_release(next);return 0;}
    /* Use owned input: in may alias the previous session's token/media data. */
    in=&next->owned.input;
    if(append) {
        xe_visual_context_release(s->visual_context);s->visual_context=NULL;
        s->visual_prompt_end=0;s->anchor_valid=0;
    } else xe_session_reset(s);
    int initial=s->n_tokens;
    size_t visual=0;
    while(visual<in->visual_count&&in->visual[visual].start+in->visual[visual].rows<=initial)visual++;
    while(s->n_tokens<in->count) {
        int start=s->n_tokens,n=in->count-start;
        const float *embedding=NULL;
        if(visual<in->visual_count&&in->visual[visual].start==start) {
            n=in->visual[visual].rows;embedding=in->visual[visual].embeddings;visual++;
        } else {
            if(visual<in->visual_count&&n>in->visual[visual].start-start)n=in->visual[visual].start-start;
            if(n>s->text_prefill_rows)n=s->text_prefill_rows;
        }
        s->visual_batch=embedding!=NULL;
        xe_prefill_batch_append_input(s,in->tokens+start,n,start,embedding);
        xe_vision_wait(s->engine);xe_prefill_batch_complete(s,start+n==in->count);
        s->visual_batch=0;
    }
    xe_session_swa_mirror(s,initial,s->n_tokens);
    s->visual_context=next;s->visual_prompt_end=in->count;xe_input_hash(in,s->visual_prompt_hash);
    if(report){report->reused=initial;report->prefilled=in->count-initial;report->restarted=!append&&previous>0;}
    return 1;
}

int xe_session_sync_input(xe_session *s,const xe_input *in) {
    return xe_session_sync_input_report(s,in,NULL);
}

int xe_session_sync_continuation(xe_session *s,const xe_tokens *prefix,xe_sync_report *report) {
    if(!s||s->cow||s->prefill_pending||!xe_tokens_valid(prefix)||prefix->len<1||prefix->len<s->n_tokens||
       prefix->len>s->engine->context)return 0;
    if(s->visual_context && s->n_tokens<s->visual_prompt_end)return 0;
    if(memcmp(s->tokens,prefix->v,(size_t)s->n_tokens*4))return 0;
    xe_session_sync_report_impl(s,prefix,report);return 1;
}

static int xe_session_rewind_input(xe_session *s,int position) {
    xe_input in=s->visual_context->owned.input;in.tokens=s->tokens;in.count=s->n_tokens;
    if(!xe_input_boundary(&in,position))return 0;
    in.count=position;in.visual_count=0;
    while(in.visual_count<s->visual_context->owned.input.visual_count &&
          in.visual[in.visual_count].start+in.visual[in.visual_count].rows<=position)in.visual_count++;
    return xe_session_sync_input(s,&in);
}

xe_snapshot_status xe_session_snapshot_load_input(xe_session *s,FILE *file,const xe_input *in) {
    if(!s||!xe_input_valid(in,s->engine->context)||s->cow||s->prefill_pending)
        return XE_SNAPSHOT_INVALID_ARGUMENT;
    xe_visual_context *next=in->visual_count?xe_visual_context_new(in):NULL;
    if(in->visual_count&&!next)return XE_SNAPSHOT_NOMEM;
    if(next)in=&next->owned.input;
    xe_tokens expected={(int32_t *)in->tokens,in->count,in->count};
    xe_snapshot_status status=xe_session_snapshot_load_impl(s,file,&expected,in);
    if(status!=XE_SNAPSHOT_OK){xe_visual_context_release(next);return status;}
    s->visual_context=next;
    if(next){s->visual_prompt_end=in->count;xe_input_hash(in,s->visual_prompt_hash);}
    return XE_SNAPSHOT_OK;
}
