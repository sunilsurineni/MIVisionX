/*
MIT License

Copyright (c) 2019 Advanced Micro Devices, Inc. All rights reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

/* This file is generated by nnir_to_clib.py on 2019-04-19T17:08:26.449949-07:00 */

#include "mvdeploy.h"
#include "vx_amd_media.h"
#include "mv_extras_postproc.h"
#include "visualize.h"
#include <iterator>

// hard coded biases for yolo_v2 and yolo_v3 (x,y) for 5 bounding boxes
const float BB_biases[10]             = {1.08,1.19,  3.42,4.41,  6.63,11.38,  9.42,5.11,  16.62,10.52};     // bounding box biases

// callback function for adding preprocessing nodes
// the module should output in the outp_tensor passed by the callback function
inline int64_t clockCounter()
{
    return std::chrono::high_resolution_clock::now().time_since_epoch().count();
}

inline int64_t clockFrequency()
{
    return std::chrono::high_resolution_clock::period::den / std::chrono::high_resolution_clock::period::num;
}

static vx_status MIVID_CALLBACK preprocess_addnodes_callback_fn(mivid_session inf_session, vx_tensor outp_tensor, const char *inp_string, float a, float b)
{
    if (inf_session) {
        mivid_handle hdl = (mivid_handle) inf_session;
        vx_context context = hdl->context;
        vx_graph graph   = hdl->graph;
        ERROR_CHECK_OBJECT(context);
        ERROR_CHECK_OBJECT(graph);
        ERROR_CHECK_STATUS(vxLoadKernels(context, "vx_amd_media"));
        // query outp_tensor for dims
        vx_size num_dims, tens_dims[4] = { 1, 1, 1, 1 };
        ERROR_CHECK_STATUS(vxQueryTensor((vx_tensor)outp_tensor, VX_TENSOR_NUMBER_OF_DIMS, &num_dims, sizeof(num_dims)));
        if (num_dims != 4) {
            printf("preprocess_addnodes_callback_fn:: outp_tensor num_dims=%ld (must be 4)\n", num_dims);
            return VX_ERROR_INVALID_DIMENSION;
        }
        ERROR_CHECK_STATUS(vxQueryTensor(outp_tensor, VX_TENSOR_DIMS, tens_dims, sizeof(tens_dims)));    
        vx_image dec_image = vxCreateImage(context, tens_dims[0], tens_dims[1]*tens_dims[3], VX_DF_IMAGE_RGB);   // todo:: add support for batch (height is tens_dims[1]*tens_dims[3])
        vx_node node_decoder = amdMediaDecoderNode(graph, inp_string, dec_image, (vx_array)nullptr);
        ERROR_CHECK_OBJECT(node_decoder);
        vx_node node_img_tensor = vxConvertImageToTensorNode(graph, dec_image, outp_tensor, a, b, 0);
        ERROR_CHECK_OBJECT(node_img_tensor);
        ERROR_CHECK_STATUS(vxReleaseNode(&node_decoder));
        ERROR_CHECK_STATUS(vxReleaseNode(&node_img_tensor));
        hdl->inp_image = dec_image;
        return VX_SUCCESS;
    } else {
        printf("preprocess_addnodes_callback_fn:: inf_session not valid\n");
        return VX_FAILURE;
    }
}

void printUsage() {
    printf("Usage: mvobjdetect <options>\n"
        "   <input-data-file>: : is filename(s) to initialize input tensor\n"
#if ENABLE_OPENCV
        "   .jpg or .png: decode and copy to raw_data file or .mp4 or .m4v for video input\n"
#endif
         "   other: initialize tensor with raw data from the file\n"
         "   <output-data-file/- > for video all frames will be output to single file OR - for no output \n"
        "   --install_folder <install_folder> : the location for compiled model\n"
        "   --backend <backend>: optional (default:OpenVX_Rocm_OpenCL) is the name of the backend for compilation\n"
        "   --frames <#num/eof> : num of frames to process inference for cases like video\n"
        "   --argmax <topK> : give argmax output in vec<label,prob>\n"
        "   --t <num of interations> to run for performance\n"
        "   --vaapi :use vaapi for decoding\n"
        "   --label <labels.txt> to run for performance\n"
        "   --bb <nc thres_c thres_nms> bounding box detection parameters\n"
        "   --v <optional (if specified visualize the result on the input image)\n"
    );
}


int main(int argc, const char ** argv)
{
    // check command-line usage
    if(argc < 5) {
        printUsage();
        return -1;
    }
    mv_status status;
    const char *inoutConfig;
    std::string labelText[1000];    
    int num_inputs=1, num_outputs=1;
    std::string install_folder = ".";
    std::string  weightsFile  = "./weights.bin";
    mivid_backend backend = OpenVX_Rocm_OpenCL;
    std::string inpFileName  = std::string(argv[1]);
    std::string outFileName  = std::string(argv[2]);
    int bPeformanceRun = 0, numIterations = 0, bVisualize = 0, bVaapi = 0;
    int detectBB = 0;
    int  argmaxOutput = 0, topK = 1;
    int useMultiFrameInput = 0, capture_till_eof = 0, start_frame=0, end_frame = 1;
    int numClasses = 0;
    float conf_th = 0.0, nms_th=0.0;
    Visualize *pVisualize = nullptr;

    for (int arg = 3; arg < argc; arg++) {
        if (!strcmp(argv[arg], "--install_folder")) {
            arg++;
            install_folder = std::string(argv[arg]);
            weightsFile = install_folder + "/" + "weights.bin";
        }
        if (!strcmp(argv[arg], "--backend")) {
            arg++;
            backend = (mivid_backend)atoi(argv[arg]);
        }        
        if (!strcmp(argv[arg], "--frames")) {
            arg++;
            useMultiFrameInput = 1;
            if (!strcmp(argv[arg], "eof")) {
                capture_till_eof = 1;
            }else {
                end_frame = atoi(argv[arg]);
            }
        }
        if (!strcmp(argv[arg], "--t")) {
            arg++;
            numIterations = atoi(argv[arg]);
        }        
        if (!strcmp(argv[arg], "--vaapi")) {
            bVaapi = atoi(argv[arg]);
        }

        if (!strcmp(argv[arg], "--visualize") || !strcmp(argv[arg], "--v")) {
            bVisualize = 1;
        }        
        if (!strcmp(argv[arg], "--argmax")) {
            arg++;
            argmaxOutput = 1;
            topK = atoi(argv[arg]);
        }

        if (!strcmp(argv[arg], "--label")) {
            if ((arg + 1) == argc)
            {
                printf("\n\nERROR: missing label.txt file on command-line (see help for details)\n\n");
                return -1;
            }            
            arg++;
            std::string labelFileName = argv[arg];
            std::string line;
            std::ifstream out(labelFileName);
            int lineNum = 0;
            while(getline(out, line)) {
                labelText[lineNum] = line;
                lineNum++;
            }
            out.close();            
        }        
        if (!strcmp(argv[arg], "--bb")) {
            if ((arg + 3) == argc)
            {
                printf("\n\nERROR: missing paramters for bounding box\n");
                return -1;
            }            
            arg++;
            detectBB = 1;
            numClasses = atoi(argv[arg++]);
            conf_th = atof(argv[arg++]);
            nms_th = atof(argv[arg]);
        }        
    }
    // initialize deployment
    if ((status = mvInitializeDeployment(install_folder.c_str()))){
        printf("ERROR: mvInitializeDeployment failed with status %d install_folder: %s \n", status, install_folder.c_str());
        return -1;
    }

    if ((status = QueryInference(&num_inputs, &num_outputs, &inoutConfig))) {
        printf("ERROR: QueryInference returned status %d\n", status);      
    }
    else {
        float *inpMem = nullptr;
        float *outMem = nullptr;
        size_t inp_dims[4], out_dims[4];
        mivid_session infSession;
        mivid_handle inf_hdl;
        mv_status status;
        vx_image inp_img;
        float time_in_millisec;
        int do_preprocess = 0;
        float scale_factor = 1.0f/255;

        // get input and output dimensions from inoutConfig
        std::stringstream inout_dims(inoutConfig);
        std::vector<std::string> config_vec;
        std::string in_names[num_inputs];
        std::string out_names[num_outputs];
        std::vector<std::tuple<int, int, int, int>> input_dims;
        std::vector<std::tuple<int, int, int, int>> output_dims;
        std::string substr;
        while( inout_dims.good()) {
            getline(inout_dims, substr, ';' );
            if (!substr.empty()) {
                config_vec.push_back(substr);
            }
        }
        int in_num = 0, out_num = 0, n, c, h, w;        
        for (int i=0; i < config_vec.size(); i++)
        {
            std::stringstream ss(config_vec[i]);
            getline(ss, substr, ',');
            if ((substr.compare(0,5,"input") == 0))
            {
                getline(ss, substr, ',');
                in_names[in_num] =  substr;
                getline(ss, substr, ','); w = atoi(substr.c_str());
                getline(ss, substr, ','); h = atoi(substr.c_str());
                getline(ss, substr, ','); c = atoi(substr.c_str());
                getline(ss, substr, ','); n = atoi(substr.c_str());
                printf("Config_input::<%d %d %d %d>:%s \n", w,h,c,n, in_names[in_num].c_str());
                input_dims.push_back(std::tuple<int,int,int,int>(w,h,c,n));
                in_num++;
            }
            else if ((substr.compare(0,6,"output") == 0))
            {
                getline(ss, substr, ',');
                out_names[out_num] =  substr;
                getline(ss, substr, ','); w = atoi(substr.c_str());
                getline(ss, substr, ','); h = atoi(substr.c_str());
                getline(ss, substr, ','); c = atoi(substr.c_str());
                getline(ss, substr, ','); n = atoi(substr.c_str());
                printf("Config_output::<%d %d %d %d>:%s \n", w,h,c,n, out_names[out_num].c_str());
                output_dims.push_back(std::tuple<int,int,int,int>(w,h,c,n));
                out_num ++;
            }            
        }

        inp_dims[3] = std::get<0>(input_dims[0]);
        inp_dims[2] = std::get<1>(input_dims[0]);
        inp_dims[1] = std::get<2>(input_dims[0]);
        inp_dims[0] = std::get<3>(input_dims[0]);
        out_dims[3] = std::get<0>(output_dims[0]);
        out_dims[2] = std::get<1>(output_dims[0]);
        out_dims[1] = std::get<2>(output_dims[0]);
        out_dims[0] = std::get<3>(output_dims[0]);

        // for video input, set preprocessing callback for adding video decoder node
        if (inp_dims[3] == 1 && inp_dims[2] == 3 && inpFileName.size() > 4 && ((inpFileName.substr(inpFileName.size()-4, 4) == ".mp4")||(inpFileName.substr(inpFileName.size()-4, 4) == ".m4v")))
        {
            std::string inp_dec_str = bVaapi? ("1," + inpFileName + ":1") : ("1," + inpFileName + ":0");
            SetPreProcessCallback(&preprocess_addnodes_callback_fn, inp_dec_str.c_str(), scale_factor, 0.0);
            do_preprocess = 1;
            printf("OK:: SetPreProcessCallback \n");
        }
        else if (inp_dims[3] > 1 && inp_dims[2] == 3 && (((inpFileName.size() > 6) && (inpFileName.substr(inpFileName.size()-6, 4) == ".mp4")
                                                        ||(inpFileName.substr(inpFileName.size()-6, 4) == ".m4v")) || (inpFileName.substr(inpFileName.size()-4, 4) == ".txt")))
        {
            std::string inp_dec_str;
            if (inpFileName.substr(inpFileName.size()-4, 4) != ".txt") {
                inp_dec_str = std::to_string(inp_dims[3]) + "," + inpFileName;
            } else {
                inp_dec_str = std::to_string(inp_dims[3]) + "," + inpFileName;
            }
            SetPreProcessCallback(&preprocess_addnodes_callback_fn, inp_dec_str.c_str(), scale_factor, 0.0);
            do_preprocess = 1;
            printf("OK:: SetPreProcessCallback \n");
        } 

        status = mvCreateInferenceSession(&infSession, install_folder.c_str(), mv_mem_type_host);
        if (status != MV_SUCCESS)
        {
            printf("ERROR: mvCreateInferenceSession returned failure \n");
            return -1;      
        }
        if (input_dims.size() == 0 || output_dims.size() == 0 ) {
            printf("ERROR: Couldn't get input and output dims %d %d \n", (int)input_dims.size(), (int)output_dims.size());
            return -1;      
        }
        inf_hdl = (mivid_handle)infSession;
        vx_context context = inf_hdl->context;
        // create input tensor memory for swaphandle
        size_t inputSizeInBytes = 4 *inp_dims[0] * inp_dims[1] * inp_dims[2] * inp_dims[3];
        inpMem = (float *)new char[inputSizeInBytes];
        size_t istride[4] = { 4, (size_t)4 * inp_dims[0], (size_t)4 * inp_dims[0] * inp_dims[1], (size_t)4 * inp_dims[0] * inp_dims[1] * inp_dims[2] };

        // check if the input file is .mp4 if yes, add preprocess callback
        Mat img, matScaled;
        Mat *inp_img_mat = nullptr;
        if (!do_preprocess) {
    #if ENABLE_OPENCV
            if(inp_dims[2] == 3 && inpFileName.size() > 4 && (inpFileName.substr(inpFileName.size()-4, 4) == ".png" || inpFileName.substr(inpFileName.size()-4, 4) == ".jpg"))
            {
                for(size_t n = 0; n < inp_dims[3]; n++) {
                    char imgFileName[1024];
                    sprintf(imgFileName, inpFileName.c_str(), (int)n);
                    unsigned char *img_data;
                    img = imread(imgFileName, CV_LOAD_IMAGE_COLOR);
                    img_data = img.data;
                    inp_img_mat = &img;
                    if(!img.data || img.rows != inp_dims[1] || img.cols != inp_dims[0]) {
                        cv::resize(img, matScaled, cv::Size(inp_dims[0], inp_dims[1]));
                        img_data = matScaled.data;
                        inp_img_mat = &matScaled;
                    }
                    for(vx_size y = 0; y < inp_dims[1]; y++) {
                        unsigned char * src = img_data + y*inp_dims[0]*3;
                        float * dstR = inpMem + ((n * istride[3] + y * istride[1]) >> 2);
                        float * dstG = dstR + (istride[2] >> 2);
                        float * dstB = dstG + (istride[2] >> 2);
                        for(vx_size x = 0; x < inp_dims[0]; x++, src += 3) {
                            *dstR++ = src[2]*scale_factor;
                            *dstG++ = src[1]*scale_factor;
                            *dstB++ = src[0]*scale_factor;
                        }
                    }
                }
            }
            else
    #endif
            {
                FILE * fp = fopen(inpFileName.c_str(), "rb");
                if(!fp) {
                    std::cerr << "ERROR: unable to open: " << inpFileName << std::endl;
                    return -1;
                }
                for(size_t n = 0; n < inp_dims[3]; n++) {
                    for(size_t c = 0; c < inp_dims[2]; c++) {
                        for(size_t y = 0; y < inp_dims[1]; y++) {
                            float * ptrY = inpMem + ((n * istride[3] + c * istride[2] + y * istride[1]) >> 2);
                            vx_size n = fread(ptrY, sizeof(float), inp_dims[0], fp);
                            if(n != inp_dims[0]) {
                                std::cerr << "ERROR: reading from file less than expected # of bytes " << inpFileName << std::endl;
                                return -1;
                            }
                        }
                    }
                }
                fclose(fp);
            }
            if ((status = mvSetInputDataFromMemory(infSession, 0, (void *)inpMem, inputSizeInBytes, mv_mem_type_host)) != MV_SUCCESS) {
                printf("ERROR: mvSetInputDataFromMemory returned failure(%d) \n", status);
                return -1;
            }
        }
        size_t outputSizeInBytes = 4 *out_dims[0]*out_dims[1]*out_dims[2]*out_dims[3];
        outMem = (float *)new char[outputSizeInBytes];
        std::vector<BBox> detected_bb;
        FILE *fp = nullptr;

        if (strcmp(outFileName.c_str(), "-") != 0)
        {
            fp = fopen(outFileName.c_str(), "wb");
            if(!fp) {
                std::cerr << "ERROR: unable to open: " << outFileName << std::endl;
                return -1;
            }
        }
        int64_t freq = clockFrequency(), t0, t1, total_time = 0;
        float time_in_ms;
        int fn; 
        // initialize postprocessing for object detection
        if (detectBB) {
            if (status = mv_postproc_init(infSession, numClasses, 13, BB_biases, 10, conf_th, nms_th, inp_dims[0], inp_dims[1])) {
                printf("ERROR: mv_postproc_init failed with status(%d) \n", status);
                return -1;
            }
        }
        if (bVisualize) {
            pVisualize = new Visualize(0.2);
        }
        ClassLabel cl[5];

        for (fn = start_frame; fn < end_frame || capture_till_eof; fn++) {
            t0 = clockCounter();
            status = mvRunInference(infSession, &time_in_millisec, numIterations);
            if (status == MV_ERROR_GRAPH_ABANDONED) break;
            if (status < 0 ) {
                printf("ERROR: mvRunInference terminated with status(%d) \n", status);
                return -1;
            }
            t1 = clockCounter();
            total_time += (t1-t0);

            // allocate output buffer corresponding to the first output

            // get output
            if ((status = mvGetOutputData(infSession, 0, (void *) outMem, outputSizeInBytes)) != MV_SUCCESS) {
                printf("ERROR: mvGetOutputData returned failure(%d) \n", status);
                return -1;
            }
            if (fp != nullptr) {
                fwrite(outMem, sizeof(float), outputSizeInBytes>>2, fp);
            }
            // do object detection of output
            if (detectBB) {
                if (status = mv_postproc_getBB_detections(infSession, outMem, out_dims[3], out_dims[2], out_dims[1], out_dims[0], detected_bb)) {
                    printf("ERROR: mv_postproc_getBB_detections returned status(%d) \n", status);
                    return -1;
                }
            }
            if (argmaxOutput) {
                mv_postproc_argmax(outMem, (void *)cl, topK, out_dims[3], out_dims[2], out_dims[1], out_dims[0]);
                for (int l=0; l<topK; l++) {
                    printf("Argmax topK: %d classs:%d conf: %f\n", l, cl[l].index, cl[l].probability);
                }
            }
            if (bVisualize && pVisualize) {
                inp_img = inf_hdl->inp_image;
                if (inp_img) {
                    vx_uint32 width, height;
                    cv::Mat img;
                    if (inp_dims[3] == 4) {
                        img.create(inp_dims[1]*2, inp_dims[0]*2, CV_8UC3);     // both width and height multiplied by 2 to accomodate 4 images
                    } else {
                        img.create(inp_dims[1]*inp_dims[3], inp_dims[0], CV_8UC3);
                    }
                    // access input image data and visualize bounding boxes on output
                    vx_imagepatch_addressing_t addr = { 0 };
                    // query for width and height
                    ERROR_CHECK_STATUS(vxQueryImage(inp_img, VX_IMAGE_WIDTH, &width, sizeof(width)));
                    ERROR_CHECK_STATUS(vxQueryImage(inp_img, VX_IMAGE_HEIGHT, &height, sizeof(height)));

                    vx_rectangle_t rect_1 = { 0, 0, width, height};
                    vx_map_id map_id;

                    vx_uint8 * src = NULL;
                    ERROR_CHECK_STATUS(vxMapImagePatch(inp_img, &rect_1, 0, &map_id, &addr, (void **)&src, VX_READ_ONLY, VX_MEMORY_TYPE_HOST, VX_NOGAP_X));
                    if (inp_dims[3] == 4) {
                        vx_uint32 height1 = height >> 2;
                        for (vx_uint32 y = 0; y < height1; y++) {
                            vx_uint8 * pDst = (vx_uint8 *)img.data + y * img.step;
                            vx_uint8 * pDst1 = pDst + width*3;
                            vx_uint8 * pSrc = (vx_uint8 *)src + y * addr.stride_y;
                            vx_uint8 * pSrc1 = (vx_uint8 *)src + (y + height1) * addr.stride_y;
                            for (vx_uint32 x = 0; x < width; x++) {
                                pDst[0] = pSrc[2];
                                pDst[1] = pSrc[1];
                                pDst[2] = pSrc[0];
                                pDst1[0] = pSrc1[2];
                                pDst1[1] = pSrc1[1];
                                pDst1[2] = pSrc1[0];
                                pDst += 3, pDst1 += 3;
                                pSrc += 3, pSrc1 += 3;
                            }
                        }
                        for (vx_uint32 y = height1; y < (height1*2); y++) {
                            vx_uint8 * pDst = (vx_uint8 *)img.data + y * img.step;
                            vx_uint8 * pDst1 = pDst + width*3;
                            vx_uint8 * pSrc = (vx_uint8 *)src + (y + height1) * addr.stride_y;
                            vx_uint8 * pSrc1 = (vx_uint8 *)src + (y + height1*2) * addr.stride_y;
                            for (vx_uint32 x = 0; x < width; x++) {
                                pDst[0] = pSrc[2];
                                pDst[1] = pSrc[1];
                                pDst[2] = pSrc[0];
                                pDst1[0] = pSrc1[2];
                                pDst1[1] = pSrc1[1];
                                pDst1[2] = pSrc1[0];
                                pDst += 3, pDst1 += 3;
                                pSrc += 3, pSrc1 += 3;
                            }
                        }
                    } else {
                        for (vx_uint32 y = 0; y < height; y++) {
                            vx_uint8 * pDst = (vx_uint8 *)img.data + y * img.step;
                            vx_uint8 * pSrc = (vx_uint8 *)src + y * addr.stride_y;
                            for (vx_uint32 x = 0; x < width; x++) {
                                pDst[0] = pSrc[2];
                                pDst[1] = pSrc[1];
                                pDst[2] = pSrc[0];
                                pDst += 3;
                                pSrc += 3;
                            }
                        }
                    }

                    ERROR_CHECK_STATUS(vxUnmapImagePatch(inp_img, map_id));
                    pVisualize->show(img, detected_bb, inp_dims[3]);
                    img.release();
                    if (cvWaitKey(1) >= 0)
                        break;
                } else if (inp_img_mat){
                    // check is we read from file to cv::img
                    pVisualize->show(*inp_img_mat, detected_bb);
                    cvWaitKey(0);
                }
            }            
        }
        
        if (fp) fclose(fp);
        if (fn) {
            time_in_ms = (float)total_time*1000.0f/(float)freq/(float)fn;
            printf("OK: mvRunInference() took %.3f msec (average over %d iterations)\n", time_in_ms, fn);            
        }
        if (pVisualize) delete pVisualize;

        if (detectBB) mv_postproc_shutdown(infSession);
        // Relese Inference
        mvReleaseInferenceSession(infSession);
        printf("OK: Inference Deploy Successful \n");
        // delete resources
        if (inpMem) delete[] inpMem;
        if (outMem) delete[] outMem;
        mvShutdown();
    }
}
