#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <float.h>
#include <conio.h>
#include "ffcnn.h"

#ifdef _MSC_VER
#pragma warning(disable:4996)
#define snprintf _snprintf
#endif

enum {
    ACTIVATE_TYPE_LINEAR,
    ACTIVATE_TYPE_RELU  ,
    ACTIVATE_TYPE_LEAKY ,
};

static float activate(float x, int type)
{
    switch (type) {
    case ACTIVATE_TYPE_RELU : return x > 0 ? x : 0;
    case ACTIVATE_TYPE_LEAKY: return x > 0 ? x : 0.1f * x;
    default: return x;
    }
}

static void matrix_fill_pad(MATRIX *mat, float val)
{
    float *data = mat->data;
    int    mw, mh, pw, ph, i, x, y;
    pw = mat->padw;
    ph = mat->padh;
    mw = mat->width + pw * 2;
    mh = mat->height+ ph * 2;
    for (i=0; i<mat->channels; i++) {
        for (y=0; y<ph; y++) {
            for (x=0; x<mw; x++) data[y * mw + x] = data[(mh - 1 - y) * mw + x] = val;
        }
        for (y=ph; y<mh-ph; y++) {
            for (x=0; x<pw; x++) data[y * mw + x] = data[y * mw + (mw - 1 - x)] = val;
        }
        data += mw * mh;
    }
}

static float filter_conv(float *mat, int mw, int x, int y, float *flt, int fw, int fh)
{
    float val = 0;
    int   i, j;
    for (j=0; j<fh; j++) {
        for (i=0; i<fw; i++) {
            val += mat[(y + j) * mw + x + i] * flt[j * fw + i];
        }
    }
    return val;
}

static float filter_avgmax(float *mat, int mw, int x, int y, int fw, int fh, int flag)
{
    float val = 0, max = mat[y * mw + x];
    int   i, j;
    for (j=0; j<fh; j++) {
        for (i=0; i<fw; i++) {
            if (flag) {
                if (max < mat[(y + j) * mw + x + i]) max = mat[(y + j) * mw + x + i];
            } else {
                val += mat[(y + j) * mw + x + i];
            }
        }
    }
    return flag ? max : val / (fw * fh);
}

static void layer_convolution_forward(LAYER *ilayer, LAYER *olayer)
{
    int  n, i, ix, iy, ox, oy, fw, fh, fs, mwi, mwo;
    float *datai, *datao, *dataf;
    fw  = ilayer->filter.width;
    fh  = ilayer->filter.height;
    fs  = ilayer->stride;
    mwi = ilayer->matrix.width + ilayer->matrix.padw * 2;
    mwo = olayer->matrix.width + olayer->matrix.padw * 2;

    datao = olayer->matrix.data + olayer->matrix.padh * mwo + olayer->matrix.padw;
    dataf = ilayer->filter.data;
    for (n=0; n<olayer->matrix.channels; n++) {
        datai = ilayer->matrix.data;
        for (i=0; i<ilayer->matrix.channels; i++) {
            for (iy=0,oy=0; iy<ilayer->matrix.width; iy+=fs,oy++) {
                for (ix=0,ox=0; ix<ilayer->matrix.width; ix+=fs,ox++) {
                    float val = filter_conv(datai, mwi, ix, iy, dataf, fw, fh);
                    if (!i) datao[oy * mwo + ox] = val;
                    else    datao[oy * mwo + ox]+= val;
                    if (i == ilayer->matrix.channels - 1) {
                        if (ilayer->batchnorm) {
                            datao[oy * mwo + ox] = (datao[oy * mwo + ox] - ilayer->filter.rolling_mean[n])/(float)sqrt(ilayer->filter.rolling_variance[n] + 0.00001f);
                            datao[oy * mwo + ox]*= ilayer->filter.scale[n];
                        }
                        datao[oy * mwo + ox]+= ilayer->filter.bias [n];
                        datao[oy * mwo + ox] = activate(datao[oy * mwo + ox], ilayer->activate);
                    }
                }
            }
            datai += (ilayer->matrix.height + ilayer->matrix.padh * 2) * mwi;
            dataf += fw * fh;
        }
        datao += (olayer->matrix.height + olayer->matrix.padh * 2) * mwo;
    }
}

static void layer_groupconv_forward(LAYER *ilayer, LAYER *olayer)
{
    LAYER tilayer, tolayer;
    int   i;
    tilayer.activate         = ilayer->activate ;
    tilayer.batchnorm        = ilayer->batchnorm;
    tilayer.filter           = ilayer->filter;
    tilayer.matrix           = ilayer->matrix;
    tilayer.stride           = ilayer->stride;
    tilayer.matrix.channels /= ilayer->groups;
    tilayer.filter.n        /= ilayer->groups;
    tolayer.matrix           = olayer->matrix;
    tolayer.matrix.channels /= ilayer->groups;
    for (i=0; i<ilayer->groups; i++) {
        layer_convolution_forward(&tilayer, &tolayer);
        tolayer.matrix.data += (tolayer.matrix.width + tolayer.matrix.padw * 2) * (tolayer.matrix.height + tolayer.matrix.padh * 2) * tilayer.filter.n;
        tilayer.matrix.data += (tilayer.matrix.width + tilayer.matrix.padw * 2) * (tilayer.matrix.height + tilayer.matrix.padh * 2) * tilayer.matrix.channels;
        tilayer.filter.data +=  tilayer.filter.width * tilayer.filter.height * tilayer.filter.channels * tilayer.filter.n;
    }
}

static void layer_avgmaxpool_forward(LAYER *ilayer, LAYER *olayer, int flag)
{
    int  n, ix, iy, ox, oy, fw, fh, fs, mwi, mwo;
    float *datai, *datao;
    fw  = ilayer->filter.width;
    fh  = ilayer->filter.height;
    fs  = ilayer->stride;
    mwi = ilayer->matrix.width + ilayer->matrix.padw * 2;
    mwo = olayer->matrix.width + olayer->matrix.padw * 2;

    datai = ilayer->matrix.data;
    datao = olayer->matrix.data + olayer->matrix.padh * mwo + olayer->matrix.padw;
    for (n=0; n<olayer->matrix.channels; n++) {
        for (iy=0,oy=0; iy<ilayer->matrix.width; iy+=fs,oy++) {
            for (ix=0,ox=0; ix<ilayer->matrix.width; ix+=fs,ox++) {
                datao[oy * mwo + ox] = activate(filter_avgmax(datai, mwi, ix, iy, fw, fh, flag), ilayer->activate);
            }
        }
        datai += (ilayer->matrix.height + ilayer->matrix.padh * 2) * mwi;
        datao += (olayer->matrix.height + olayer->matrix.padh * 2) * mwo;
    }
}

static void layer_upsample_forward(LAYER *ilayer, LAYER *olayer)
{
    int    mwi   = ilayer->matrix.width + ilayer->matrix.padw * 2;
    int    mwo   = olayer->matrix.width + olayer->matrix.padw * 2;
    float *datai = ilayer->matrix.data + ilayer->matrix.padh * mwi + ilayer->matrix.padw;
    float *datao = olayer->matrix.data + olayer->matrix.padh * mwo + olayer->matrix.padw;
    int    stride= ilayer->stride, i, x, y;
    for (i=0; i<ilayer->matrix.channels; i++) {
        for (y=0; y<olayer->matrix.height; y++) {
            for (x=0; x<olayer->matrix.width; x++) {
                datao[y * mwo + x] = datai[(y / stride) * mwi + (x / stride)];
            }
        }
        datai += (ilayer->matrix.height + ilayer->matrix.padh * 2) * mwi;
        datao += (olayer->matrix.height + olayer->matrix.padh * 2) * mwo;
    }
}

static void layer_dropout_forward(LAYER *ilayer, LAYER *olayer)
{
    olayer->matrix.data = ilayer->matrix.data;
    ilayer->matrix.data = NULL;
}

static void layer_shortcut_forward(LAYER *head, LAYER *ilayer, LAYER *olayer)
{
    LAYER  *slayer = head + ilayer->depend_list[0] + 1;
    MATRIX *mr = &olayer->matrix, *m1 = &ilayer->matrix, *m2 = &slayer->matrix;
    int     mwr= mr->width + mr->padw * 2;
    int     mw1= m1->width + m1->padw * 2;
    int     mw2= m2->width + m2->padw * 2;
    float  *datar = mr->data + mr->padh * mwr + mr->padw;
    float  *data1 = m1->data + m1->padh * mw1 + m1->padw;
    float  *data2 = m2->data + m2->padh * mw2 + m2->padw;
    int     i, x, y;
    for (i=0; i<mr->channels; i++) {
        for (y=0; y<mr->height; y++) {
            for (x=0; x<mr->width; x++) {
                datar[y * mwr + x] = activate(data1[y * mw1 + x] + data2[y * mw2 + x], ilayer->activate);
            }
        }
        datar += (mr->height + mr->padh * 2) * mwr;
        data1 += (m1->height + m1->padh * 2) * mw1;
        data2 += (m2->height + m2->padh * 2) * mw2;
    }
}

static void layer_route_forward(LAYER *head, LAYER *ilayer, LAYER *olayer)
{
    int    mwo   = olayer->matrix.width + olayer->matrix.padw * 2;
    float *datao = olayer->matrix.data + olayer->matrix.padh * mwo + olayer->matrix.padw;
    int  i, j, k;
    for (i=0; i<ilayer->depend_num; i++) {
        LAYER *rlayer = head + ilayer->depend_list[i] + 1;
        int    mwr    = rlayer->matrix.width + rlayer->matrix.padw * 2;
        float *datar  = rlayer->matrix.data + rlayer->matrix.padh * mwr + rlayer->matrix.padw;
        for (j=0; j<rlayer->matrix.channels; j++) {
            for (k=0; k<rlayer->matrix.height; k++) memcpy(datao + k * mwo, datar + k * mwr, olayer->matrix.width * sizeof(float));
            datao += mwo * (olayer->matrix.height + olayer->matrix.padh * 2);
            datar += mwr * (rlayer->matrix.height + rlayer->matrix.padh * 2);
        }
    }
}

static void layer_forward(LAYER *head, LAYER *ilayer, LAYER *olayer)
{
    switch (ilayer->type) {
    case LAYER_TYPE_CONV    : layer_groupconv_forward (ilayer, olayer);       break;
    case LAYER_TYPE_AVGPOOL : layer_avgmaxpool_forward(ilayer, olayer, 0);    break;
    case LAYER_TYPE_MAXPOOL : layer_avgmaxpool_forward(ilayer, olayer, 1);    break;
    case LAYER_TYPE_UPSAMPLE: layer_upsample_forward  (ilayer, olayer);       break;
    case LAYER_TYPE_DROPOUT : layer_dropout_forward   (ilayer, olayer);       break;
    case LAYER_TYPE_SHORTCUT: layer_shortcut_forward  (head, ilayer, olayer); break;
    case LAYER_TYPE_ROUTE   : layer_route_forward     (head, ilayer, olayer); break;
    }
}

static char* load_file_to_buffer(char *file)
{
    FILE *fp = fopen(file, "rb");
    char *buf= NULL;
    int   size;
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    buf  = malloc(size + 1);
    if (buf) {
        fread(buf, 1, size, fp);
        buf[size] = '\0';
    }
    fclose(fp);
    return buf;
}

static int get_total_layers(char *str)
{
    static const char *STRTAB_LAYER_TYPE[] = {
        "[conv]", "[convolutional]", "[avg]", "[avgpool]", "[max]", "[maxpool]", "[upsample]", "[dropout]", "[shortcut]", "[route]", "[yolo]", NULL,
    };
    int n = 0, i;
    while (str && (str = strstr(str, "["))) {
        for (i=0; STRTAB_LAYER_TYPE[i]; i++) {
            if (strstr(str, STRTAB_LAYER_TYPE[i]) == str) break;
        }
        if (STRTAB_LAYER_TYPE[i]) n++;
        str = strstr(str, "]");
    }
    return n;
}

static char* parse_params(const char *str, const char *end, const char *key, char *val, int len)
{
    char *p = (char*)strstr(str, key);
    int   i;

    *val = '\0';
    if (!p || (end && p >= end)) return NULL;
    p += strlen(key);
    if (*p == '\0') return NULL;

    while (*p) {
        if (*p != '=' && *p != ' ') break;
        else p++;
    }

    for (i=0; i<len; i++) {
        if (*p == '\n' || *p == '\0') break;
        val[i] = *p++;
    }
    val[i < len ? i : len - 1] = '\0';
    return val;
}

static int get_activation_type_int(char *str)
{
    static const char *STR_TAB[] = { "linear", "relu", "leaky", NULL };
    int  i;
    for (i=0; STR_TAB[i]; i++) {
        if (strstr(str, STR_TAB[i]) == str) return i;
    }
    return -1;
}

static char* get_activation_type_string(int type)
{
    static const char *STR_TAB[] = { "linear", "relu", "leaky", NULL };
    return (type >= 0 && type <= 2) ? (char*)STR_TAB[type] : "unknown";
}

static char* get_layer_type_string(int type)
{
    static const char *STR_TAB[] = { "conv", "avgpool", "maxpool", "upsample", "dropout", "shortcut", "route", "yolo" };
    return (type >= 0 && type <= 7) ? (char*)STR_TAB[type] : "unknown";
}

static void calculate_output_whc(LAYER *in, LAYER *out)
{
    in ->matrix.padw     = in->pad ? in->filter.width / 2 : 0;
    in ->matrix.padh     = in->pad ? in->filter.height/ 2 : 0;
    out->matrix.channels =  in->filter.n;
    out->matrix.width    = (in->matrix.width - in->filter.width + in->matrix.padw * 2) / in->stride + 1;
    out->matrix.height   = (in->matrix.height- in->filter.height+ in->matrix.padh * 2) / in->stride + 1;
}

NET* net_load(char *file1, char *file2)
{
    char *cfgstr = load_file_to_buffer(file1), *pstart, *pend, strval[256];
    NET  *net    = NULL;
    int   layers, layercur = 0, i;
    if (!cfgstr) return NULL;

    layers = get_total_layers(cfgstr);
    net    = calloc(1, sizeof(NET) + (layers + 1) * sizeof(LAYER));
    pstart = cfgstr;
    net->layer_list = (LAYER*)((char*)net + sizeof(NET));
    net->layer_num  = layers;

    while (pstart && (pstart = strstr(pstart, "["))) {
        pend = strstr(pstart + 1, "[");
        if (pend) pend = pend - 1;

        if (strstr(pstart, "[net]") == pstart) {
            parse_params(pstart, pend, "width"   , strval, sizeof(strval)); net->layer_list[0].matrix.width   = atoi(strval);
            parse_params(pstart, pend, "height"  , strval, sizeof(strval)); net->layer_list[0].matrix.height  = atoi(strval);
            parse_params(pstart, pend, "channels", strval, sizeof(strval)); net->layer_list[0].matrix.channels= atoi(strval);
        } else if (strstr(pstart, "[conv]") == pstart || strstr(pstart, "[convolutional]") == pstart) {
            parse_params(pstart, pend, "filters" , strval, sizeof(strval)); net->layer_list[layercur].filter.n = atoi(strval);
            parse_params(pstart, pend, "size"    , strval, sizeof(strval)); net->layer_list[layercur].filter.width = net->layer_list[layercur].filter.height = atoi(strval);
            parse_params(pstart, pend, "stride"  , strval, sizeof(strval)); net->layer_list[layercur].stride = atoi(strval);
            parse_params(pstart, pend, "pad"     , strval, sizeof(strval)); net->layer_list[layercur].pad    = atoi(strval);
            parse_params(pstart, pend, "groups"  , strval, sizeof(strval)); net->layer_list[layercur].groups = atoi(strval);
            parse_params(pstart, pend, "batch_normalize", strval, sizeof(strval)); net->layer_list[layercur].batchnorm = atoi(strval);
            parse_params(pstart, pend, "activation"     , strval, sizeof(strval)); net->layer_list[layercur].activate  = get_activation_type_int(strval);
            if (net->layer_list[layercur].stride== 0) net->layer_list[layercur].stride= 1;
            if (net->layer_list[layercur].groups== 0) net->layer_list[layercur].groups= 1;
            net->layer_list[layercur].filter.channels = net->layer_list[layercur].matrix.channels / net->layer_list[layercur].groups;
            net->weight_size += net->layer_list[layercur].filter.width * net->layer_list[layercur].filter.height * net->layer_list[layercur].filter.channels * net->layer_list[layercur].filter.n;
            net->weight_size += net->layer_list[layercur].filter.n * (1 + !!net->layer_list[layercur].batchnorm * 3);
            net->layer_list[layercur++].type = LAYER_TYPE_CONV;
            calculate_output_whc(net->layer_list + layercur - 1, net->layer_list + layercur);
        } else if (strstr(pstart, "[avg]") == pstart || strstr(pstart, "[avgpool]") == pstart || strstr(pstart, "[max]") == pstart || strstr(pstart, "[maxpool]") == pstart) {
            parse_params(pstart, pend, "size"  , strval, sizeof(strval)); net->layer_list[layercur].filter.width = net->layer_list[layercur].filter.height = atoi(strval);
            parse_params(pstart, pend, "stride", strval, sizeof(strval)); net->layer_list[layercur].stride = atoi(strval);
            parse_params(pstart, pend, "pad"   , strval, sizeof(strval)); net->layer_list[layercur].pad = (strcmp(strval, "") == 0) ? 1 : atoi(strval);
            net->layer_list[layercur  ].filter.n = net->layer_list[layercur].matrix.channels;
            net->layer_list[layercur++].type = (strstr(pstart, "[avg") == pstart) ? LAYER_TYPE_AVGPOOL : LAYER_TYPE_MAXPOOL;
            calculate_output_whc(net->layer_list + layercur - 1, net->layer_list + layercur);
        } else if (strstr(pstart, "[upsample]") == pstart) {
            parse_params(pstart, pend, "stride" , strval, sizeof(strval)); net->layer_list[layercur].stride = atoi(strval);
            net->layer_list[layercur+1].matrix.channels = net->layer_list[layercur].matrix.channels;
            net->layer_list[layercur+1].matrix.width    = net->layer_list[layercur].matrix.width  * net->layer_list[layercur].stride;
            net->layer_list[layercur+1].matrix.height   = net->layer_list[layercur].matrix.height * net->layer_list[layercur].stride;
            net->layer_list[layercur++].type = LAYER_TYPE_UPSAMPLE;
        } else if (strstr(pstart, "[dropout]") == pstart || strstr(pstart, "[shortcut]") == pstart) {
            if (strstr(pstart, "[dropout]") == pstart) {
                net->layer_list[layercur++].type = LAYER_TYPE_DROPOUT;
            } else {
                parse_params(pstart, pend, "from"      , strval, sizeof(strval)); net->layer_list[layercur].depend_list[0] = atoi(strval) + layercur;
                parse_params(pstart, pend, "activation", strval, sizeof(strval)); net->layer_list[layercur].activate = get_activation_type_int(strval);
                net->layer_list[layercur  ].depend_num = 1;
                net->layer_list[layercur++].type = LAYER_TYPE_SHORTCUT;
            }
            net->layer_list[layercur].matrix.channels = net->layer_list[layercur - 1].matrix.channels;
            net->layer_list[layercur].matrix.width    = net->layer_list[layercur - 1].matrix.width;
            net->layer_list[layercur].matrix.height   = net->layer_list[layercur - 1].matrix.height;
        } else if (strstr(pstart, "[route]") == pstart) {
            char *str; int n = 0, dep = 0;
            parse_params(pstart, pend, "layers", strval, sizeof(strval));
            while (n < 4 && (str = strtok(n ? NULL : strval, ","))) {
                dep = atoi(str);
                dep = dep > 0 ? dep : layercur + dep;
                net->layer_list[layercur + 0].depend_list[n++] = dep;
                net->layer_list[layercur + 1].matrix.channels += net->layer_list[dep + 1].matrix.channels;
                net->layer_list[layercur + 1].matrix.width     = net->layer_list[dep + 1].matrix.width;
                net->layer_list[layercur + 1].matrix.height    = net->layer_list[dep + 1].matrix.height;
            }
            net->layer_list[layercur].depend_num = n;
            net->layer_list[layercur++].type = LAYER_TYPE_ROUTE;
        } else if (strstr(pstart, "[yolo]") == pstart) {
            net->layer_list[layercur++].type = LAYER_TYPE_YOLO;
        }
        pstart = pend;
    }
    free(cfgstr);

    net->weight_buf = malloc(net->weight_size * sizeof(float));
    if (net->weight_buf) {
        float *pfloat = net->weight_buf;
        for (i=0; i<layers; i++) {
            if (net->layer_list[i].type == LAYER_TYPE_CONV) {
                FILTER *filter = &net->layer_list[i].filter;
                filter->bias = pfloat; pfloat += filter->n;
                if (net->layer_list[i].batchnorm) {
                    filter->scale            = pfloat; pfloat += filter->n;
                    filter->rolling_mean     = pfloat; pfloat += filter->n;
                    filter->rolling_variance = pfloat; pfloat += filter->n;
                }
                filter->data = pfloat; pfloat += filter->width * filter->height * filter->channels * filter->n;
            }
        }
    }
    return net;
}

void net_free(NET *net)
{
    int  i;
    if (!net) return;
    for (i=0; i<net->layer_num+1; i++) {
        if (net->layer_list[i].matrix.data) {
            printf("net_free, free matrix memory for layer %d\n", i);
            free(net->layer_list[i].matrix.data);
        }
    }
    free(net->weight_buf);
    free(net);
}

void net_input(NET *net, unsigned char *bgr, int w, int h, float *mean, float *norm)
{
    MATRIX *mat = NULL;
    int    sw, sh, s1, s2, i, j;
    float  *p1, *p2, *p3;
    if (!net) return;

    mat = &(net->layer_list[0].matrix);
    if (mat->channels != 3) { printf("invalid input matrix channels: %d !\n", mat->channels); return; }
    if (!mat->data) {
        mat->data = malloc((mat->width + mat->padw * 2) * (mat->height + mat->padh * 2) * mat->channels * sizeof(float));
        if (!mat->data) { printf("failed to allocate memory for net input !\n"); return; }
        else matrix_fill_pad(mat, 0);
    }

    if (w * mat->height > h * mat->width) {
        sw = mat->width;
        sh = mat->width * h / w;
        s1 = w;
        s2 = sw;
    } else {
        sh = mat->height;
        sw = mat->height* w / h;
        s1 = h;
        s2 = sh;
    }
    p1 = mat->data;
    p2 = mat->data + 1 * mat->width * mat->height;
    p3 = mat->data + 2 * mat->width * mat->height;
    for (i=0; i<sh; i++) {
        for (j=0; j<sw; j++) {
            int x, y, r, g, b;
            x = j * s1 / s2;
            y = i * s1 / s2;
            b = bgr[y * w * 3 + x * 3 + 0];
            g = bgr[y * w * 3 + x * 3 + 1];
            r = bgr[y * w * 3 + x * 3 + 2];
            p1[(mat->padh + i) * (mat->width + mat->padw * 2) + mat->padw + j] = (b - mean[0]) * norm[0];
            p2[(mat->padh + i) * (mat->width + mat->padw * 2) + mat->padw + j] = (b - mean[1]) * norm[1];
            p3[(mat->padh + i) * (mat->width + mat->padw * 2) + mat->padw + j] = (b - mean[2]) * norm[2];
        }
    }

    for (i=0; i<net->layer_num; i++) {
        if (net->layer_list[i].depend_num > 0) {
            for (j=0; j<net->layer_list[i].depend_num; j++) {
                net->layer_list[net->layer_list[i].depend_list[j] + 1].refcnt++;
            }
        }
    }
}

void net_forward(NET *net)
{
    LAYER *ilayer, *olayer;
    int  i, j;
    if (!net) return;
    for (i=0; i<net->layer_num; i++) {
        ilayer = net->layer_list + i + 0;
        olayer = net->layer_list + i + 1;
        if (!olayer->matrix.data && ilayer->type != LAYER_TYPE_DROPOUT) {
            olayer->matrix.data = malloc((olayer->matrix.width + olayer->matrix.padw * 2) * (olayer->matrix.height + olayer->matrix.padh * 2) * olayer->matrix.channels * sizeof(float));
            if (!olayer->matrix.data) { printf("failed to allocate memory for output layer !\n"); return; }
            else matrix_fill_pad(&olayer->matrix, olayer->type == LAYER_TYPE_MAXPOOL ? -FLT_MAX : 0);
        }

        layer_forward(net->layer_list, ilayer, olayer);

        if (ilayer->refcnt == 0) {
            free(ilayer->matrix.data);
            ilayer->matrix.data = NULL;
        }
        for (j=0; j<ilayer->depend_num; j++) {
            if (--net->layer_list[ilayer->depend_list[j] + 1].refcnt == 0) {
                free(net->layer_list[ilayer->depend_list[j] + 1].matrix.data);
                net->layer_list[ilayer->depend_list[j] + 1].matrix.data = NULL;
            }
        }
    }
}

void net_dump(NET *net)
{
    int i, j;
    if (!net) return;
    printf("layer   type  filters fltsize  pad/strd input          output       bn/act  ref\n");
    for (i=0; i<net->layer_num; i++) {
        if (net->layer_list[i].type == LAYER_TYPE_YOLO) {
            printf("%3d %8s\n", i, get_layer_type_string(net->layer_list[i].type));
        } else if (net->layer_list[i].type == LAYER_TYPE_DROPOUT) {
            printf("%3d %8s %-38s -> %3dx%3dx%3d\n", i, get_layer_type_string(net->layer_list[i].type), "",
                net->layer_list[i+1].matrix.width, net->layer_list[i+1].matrix.height, net->layer_list[i+1].matrix.channels);
        } else if (net->layer_list[i].type == LAYER_TYPE_SHORTCUT || net->layer_list[i].type == LAYER_TYPE_ROUTE) {
            char strdeps[256] = "layers:", strnum[16];
            for (j=0; j<net->layer_list[i].depend_num; j++) {
                snprintf(strnum, sizeof(strnum), " %d", net->layer_list[i].depend_list[j]);
                strncat(strdeps, strnum, sizeof(strdeps) - 1);
            }
            printf("%3d %8s %-38s -> %3dx%3dx%3d           %d\n", i, get_layer_type_string(net->layer_list[i].type), strdeps,
                net->layer_list[i+1].matrix.width, net->layer_list[i+1].matrix.height, net->layer_list[i+1].matrix.channels, net->layer_list[i].refcnt);
        } else {
            printf("%3d %8s %3d/%3d %2dx%2dx%3d   %d/%2d   %3dx%3dx%3d -> %3dx%3dx%3d  %d/%-6s %d\n", i,
                get_layer_type_string(net->layer_list[i].type), net->layer_list[i].filter.n, net->layer_list[i].groups,
                net->layer_list[i].filter.width, net->layer_list[i].filter.height, net->layer_list[i].filter.channels,
                net->layer_list[i].pad, net->layer_list[i].stride,
                net->layer_list[i+0].matrix.width, net->layer_list[i+0].matrix.height, net->layer_list[i+0].matrix.channels,
                net->layer_list[i+1].matrix.width, net->layer_list[i+1].matrix.height, net->layer_list[i+1].matrix.channels,
                net->layer_list[i].batchnorm, get_activation_type_string(net->layer_list[i].activate), net->layer_list[i].refcnt);
        }
    }
    printf("total weights: %d floats, %d bytes\n", net->weight_size, net->weight_size * sizeof(float));
}

int main(int argc, char *argv[])
{
    char *file_cfg    = "yolo-fastest-1.1.cfg";
    char *file_weight = "yolo-fastest-1.1.weight";
    char  bgr[640 * 480 * 3];
    float MEAN[3] = { 0.0f, 0.0f, 0.0f };
    float NORM[3] = { 1/255.f, 1/255.f, 1/255.f };
    NET  *net = NULL;

    if (argc > 1) file_cfg    = argv[1];
    if (argc > 2) file_weight = argv[2];

    net = net_load(file_cfg, file_weight);
    net_input  (net, bgr, 640, 480, MEAN, NORM);
    net_forward(net);
    net_dump   (net);
    net_free   (net);
    getch();
    return 0;
}
