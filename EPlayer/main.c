//
//  main.c
//  EPlayer
//
//  Created by zhaofei on 2020/8/21.
//  Copyright © 2020 zhaofei. All rights reserved.
//

#include "header.h"

void set_av_log_level_and_check_ffmpeg_version() {
    av_log_set_level(AV_LOG_DEBUG);
    const char *version_info = av_version_info();
//    av_log(NULL, AV_LOG_INFO, "FFmpeg version info: %s\n", version_info);
}

void check_sdl_version() {
    // SDL
    SDL_version compiled;
    SDL_version linked;
    
    SDL_VERSION(&compiled);
    SDL_GetVersion(&linked);
    
    SDL_Log("We compiled against SDL version %d.%d.%d ...\n", compiled.major, compiled.minor, compiled.patch);
    SDL_Log("But we are linking against SDL version %d.%d.%d.\n", linked.major, linked.minor, linked.patch);
}

void error_callback(int error, const char *description) {
    printf("error_callback: error: %d, description: %s\n", error, description);
}

/*
 每当窗口改变大小，GLFW会调用这个函数并填充相应的参数供你处理
 当窗口被第一次显示的时候framebuffer_size_callback也会被调用
 */
void frame_buffersize_callback(GLFWwindow *window, int width, int height) {
    glViewport(0, 0, width, height);
    
}

// 输入处理函数
void processInput(GLFWwindow* window) {
    if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
        glfwSetWindowShouldClose(window, 1);
    }
}

float mixValue = 0.2;

const char *vertexShaderSource = "#version 330 core\n"
    "layout (location = 0) in vec3 aPos;\n"
    "layout (location = 1) in vec3 aColor;\n"
    "layout (location = 2) in vec2 aTexCoord;\n"
    "out vec3 outColor;\n"
    "out vec2 TexCoord;\n"
    "void main()\n"
    "{\n"
    "   gl_Position = vec4(aPos.x, -aPos.y, aPos.z, 1.0);\n" // 翻转Y
    "   outColor = aColor;\n"
    "   TexCoord = aTexCoord;\n"
    "}\n";

const char *fragmentShaderSource = "#version 330 core\n"
    "out vec4 FragColor;\n"
    "in vec3 outColor;\n"
    "in vec2 TexCoord;\n"
    "uniform float mixValue;\n"
    "uniform sampler2D texture1;\n"
    "void main()\n"
    "{\n"
    "   FragColor = texture(texture1, TexCoord);\n"
    "}\n";

// * vec4(outColor, 1.0)

int main(int argc, const char * argv[]) {
    
    outfile = fopen(out_file_name, "wb");
        
    int width = 1920;
    int height = 1080;
    
    int success = 0;
    char infoLog[1024] = {0, };
    
    const char *input_filename = "/Users/zhaofei/Desktop/bunny_1080p_60fps.mp4";
//    "/Users/zhaofei/Desktop/bunny_1080p_60fps.mp4";
//    "/Users/zhaofei/Desktop/EASYLIFE TEST CARD AUDIO VIDEO SYNC 16_9 10 min Ver2 1080 30FPS.mp4";
//    bunny_1080p_60fps.mp4";
//    "/Users/zhaofei/Desktop/small_bunny_1080p_30fps.mp4";
//    "/Users/zhaofei/Desktop/filter.aac";
//    "http://ivi.bupt.edu.cn/hls/cctv1hd.m3u8";
//    "/Users/zhaofei/Desktop/out.mp3";
    
    set_av_log_level_and_check_ffmpeg_version();
    check_sdl_version();
    
    av_init_packet(&flush_pkt);
    flush_pkt.data = (uint8_t *)&flush_pkt;
    
    PlayState *is;
    is = stream_open(input_filename, NULL);
    
    GLFWwindow *window;
    int vertexShader;
    int fragmentShader;
    int shaderProgram;
    
    // 初始化
    unsigned int VBO, VAO, EBO;
    
    // 纹理
    unsigned int texture;
    
    float vertices[] = {
        // --- 位置 ---          --- 颜色 ---      --- 纹理坐标 ---
        1.0f,   1.0f, 0.0f,  1.0f, 0.0f, 0.0f,      1.0f, 1.0f,                      // right top
        1.0f,   -1.0f, 0.0f,  0.0f, 1.0f, 0.0f,      1.0f, 0.0f,                      // right bottom
        -1.0f,   -1.0f, 0.0f,  0.0f, 0.0f, 1.0f,      0.0f, 0.0f,                       // left bottom
        -1.0f,   1.0f, 0.0f,  1.0f, 1.0f, 0.0f,      0.0f, 1.0f,                      // left top
    };
    
    unsigned int indices[] = {
        0, 1, 3,
        1, 2, 3
    };
    
    if (!glfwInit()) {
        printf("glfw init failed!\n");
        return 0;
    }
    
    // 告诉GLFW我们要使用的OpenGL版本是3.3
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    // 告诉GLFW我们使用的是核心模式(Core-profile)
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    
    // MAC OS
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
    
    glfwWindowHint(GLFW_SAMPLES, 4);
    
    glfwSetErrorCallback(error_callback);
    
    window = glfwCreateWindow(width, height, "Hello world", NULL, NULL);
    if (!window) {
        printf("create window failed\n");
        goto __Destroy;
    }
    
    glfwMakeContextCurrent(window);
    
    // GLAD是用来管理OpenGL的函数指针的, 所以在调用任何OpenGL的函数之前我们需要初始化GLAD
    // glad初始化
    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        printf("加载失败");
        goto __Destroy;
    }
    
    glfwSetFramebufferSizeCallback(window, frame_buffersize_callback);
    

    // vertex shafer
    vertexShader = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vertexShader, 1, &vertexShaderSource, NULL);
    glCompileShader(vertexShader);
    // check
    glGetShaderiv(vertexShader, GL_COMPILE_STATUS, &success);
    if (!success) {
        glGetShaderInfoLog(vertexShader, 1024, NULL, infoLog);
        printf("vertexShader glGetShaderiv error: %s\n", infoLog);
        goto __Destroy;
    }

    // fragment shader
    fragmentShader = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fragmentShader, 1, &fragmentShaderSource, NULL);
    glCompileShader(fragmentShader);
    // check
    glGetShaderiv(fragmentShader, GL_COMPILE_STATUS, &success);
    if (!success) {
        glGetShaderInfoLog(fragmentShader, 1024, NULL, infoLog);
        printf("fragmentShader glGetShaderiv error: %s\n", infoLog);
        goto __Destroy;
    }

    // link shaders
    shaderProgram = glCreateProgram();
    glAttachShader(shaderProgram, vertexShader);
    glAttachShader(shaderProgram, fragmentShader);
    glLinkProgram(shaderProgram);
    // check
    glGetProgramiv(shaderProgram, GL_LINK_STATUS, &success);
    if (!success) {
        glGetShaderInfoLog(shaderProgram, 1024, NULL, infoLog);
        printf("shaderProgram glGetProgramiv error: %s\n", infoLog);
        goto __Destroy;
    }
    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);

    glGenVertexArrays(1, &VAO);
    glGenBuffers(1, &VBO);
    glGenBuffers(1, &EBO);

    // 绑定VAO
    glBindVertexArray(VAO);

    // 把顶点数组复制到缓冲中, 供OpenGL使用
    glBindBuffer(GL_ARRAY_BUFFER, VBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, EBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);

    // 顶点
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 8 *sizeof(float), (void *)0);
    glEnableVertexAttribArray(0);

    // 颜色
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 8 *sizeof(float), (void *)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);

    // 纹理坐标
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 8 *sizeof(float), (void *)(6 * sizeof(float)));
    glEnableVertexAttribArray(2);

    // texture1
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    // 纹理环绕方式
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    // 当进行放大 Magnify 和 缩小 Minify 操作, 设置的纹理过滤选项, 领近过滤/线性过滤
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);

    uint8_t *frame_data;
    frame_data = malloc(width*height*4);

    while (!glfwWindowShouldClose(window)) {
        // 输入处理
        processInput(window);
        
        // 渲染指令
        glClearColor(0.2f, 0.3f, 0.3f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        
//        double remaining_time = 0.0;
        if (video_refresh(is, &remaining_time, frame_data) >= 0) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, frame_data);
            glGenerateMipmap(GL_TEXTURE_2D);
        }
        
        glUseProgram(shaderProgram);
        glBindVertexArray(VAO);
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);

        
        //glfwSwapBuffers函数会交换颜色缓冲（它是一个储存着GLFW窗口每一个像素颜色值的大缓冲），它在这一迭代中被用来绘制，并且将会作为输出显示在屏幕上
        glfwSwapBuffers(window);
        // 事件处理
        glfwPollEvents();
    }
    
__Destroy:
    
    if (outfile) {
        fclose(outfile);
    }
    
    return 0;
}
