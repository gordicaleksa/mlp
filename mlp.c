#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <assert.h>
#include <string.h>

// -----------------------------------------------------------------------------
// Helper functions

extern inline FILE *fopen_check(const char *path, const char *mode, const char *file, int line) {
    FILE *fp = fopen(path, mode);
    if (fp == NULL) {
        fprintf(stderr, "Error: Failed to open file '%s' at %s:%d\n", path, file, line);
        fprintf(stderr, "Error details:\n");
        fprintf(stderr, "  File: %s\n", file);
        fprintf(stderr, "  Line: %d\n", line);
        fprintf(stderr, "  Path: %s\n", path);
        fprintf(stderr, "  Mode: %s\n", mode);
        fprintf(stderr, "---> HINT 1: dataset files/code have moved to dev/data recently (May 20, 2024). You may have to mv them from the legacy data/ dir to dev/data/(dataset), or re-run the data preprocessing script. Refer back to the main README\n");
        fprintf(stderr, "---> HINT 2: possibly try to re-run `python train_gpt2.py`\n");
        exit(EXIT_FAILURE);
    }
    return fp;
}

#define fopenCheck(path, mode) fopen_check(path, mode, __FILE__, __LINE__)

extern inline void fclose_check(FILE *fp, const char *file, int line) {
    if (fclose(fp) != 0) {
        fprintf(stderr, "Error: Failed to close file at %s:%d\n", file, line);
        fprintf(stderr, "Error details:\n");
        fprintf(stderr, "  File: %s\n", file);
        fprintf(stderr, "  Line: %d\n", line);
        exit(EXIT_FAILURE);
    }
}

#define fcloseCheck(fp) fclose_check(fp, __FILE__, __LINE__)

extern inline void *malloc_check(size_t size, const char *file, int line) {
    void *ptr = malloc(size);
    if (ptr == NULL) {
        fprintf(stderr, "Error: Memory allocation failed at %s:%d\n", file, line);
        fprintf(stderr, "Error details:\n");
        fprintf(stderr, "  File: %s\n", file);
        fprintf(stderr, "  Line: %d\n", line);
        fprintf(stderr, "  Size: %zu bytes\n", size);
        exit(EXIT_FAILURE);
    }
    return ptr;
}

#define mallocCheck(size) malloc_check(size, __FILE__, __LINE__)

extern inline void *calloc_check(size_t nmemb, size_t size, const char *file, int line) {
    void *ptr = calloc(nmemb, size);
    if (ptr == NULL) {
        fprintf(stderr, "Error: Memory allocation failed at %s:%d\n", file, line);
        fprintf(stderr, "Error details:\n");
        fprintf(stderr, "  File: %s\n", file);
        fprintf(stderr, "  Line: %d\n", line);
        fprintf(stderr, "  Size: %zu bytes\n", nmemb * size);
        exit(EXIT_FAILURE);
    }
    return ptr;
}

#define callocCheck(nmemb, size) calloc_check(nmemb, size, __FILE__, __LINE__)

// -----------------------------------------------------------------------------
// MLP model

// Define the structure for the MLP
typedef struct {
    // config
    int vocab_size;
    int context_length;
    int embedding_size;
    int hidden_size;
    int batch_size;
    // model parameters
    // TODO(gordicaleksa): in the end prefix this with "w_" to denote weights
    float *wte;
    float *fc1_weights;
    float *fc1_bias;
    float *fc2_weights;
    float *fc2_bias;
    size_t num_parameters;
    // activations
    float *act_emb;
    float *act_h;
    float *act_logits;
    float *act_probs;
    // forward pass
    int* inputs; // the input tokens for the current forward pass
    int* targets; // the target tokens for the current forward pass
    // backward pass
    float* grad_logits;
    float* grad_fc2_weights;
    float* grad_fc2_bias;
    float* grad_h;
    float* grad_fc1;
    float* grad_fc1_weights;
    float* grad_fc1_bias;
    float* grad_emb;
    float* grad_wte;
} MLP;

float* read_parameter_array(FILE *file, int size) {
    float *array = (float*)mallocCheck(size * sizeof(float));
    freadCheck(array, sizeof(float), size, file);
    return array;
}

MLP* mlp_build_from_checkpoint(MLP *model, const char *filename) {
    // read in model from a checkpoint file
    FILE *model_file = fopenCheck(filename, "rb");
    int model_header[256];
    freadCheck(model_header, sizeof(int), 256, model_file);
    if (model_header[0] != 20240719) { printf("Bad magic model file\n"); exit(1); }

    // read in hyperparameters
    model->vocab_size = model_header[1];
    model->context_length = model_header[2];
    model->embedding_size = model_header[3];
    model->hidden_size = model_header[4];
    printf("[MLP]\n");
    printf("vocab_size: %d\n", model->vocab_size);
    printf("context_length: %d\n", model->context_length);
    printf("embedding_size: %d\n", model->embedding_size);
    printf("hidden_size: %d\n", model->hidden_size);

    int wte_size = model->vocab_size * model->embedding_size;
    int fc1_weights_size = model->hidden_size * model->embedding_size * model->context_length;
    int fc1_bias_size = model->hidden_size;
    int fc2_weights_size = model->vocab_size * model->hidden_size;
    int fc2_bias_size = model->vocab_size;
    size_t num_parameters = wte_size + fc1_weights_size + fc1_bias_size + fc2_weights_size + fc2_bias_size;
    printf("num_parameters: %zu\n", num_parameters);
    model->num_parameters = num_parameters;

    model->wte = read_parameter_array(model_file, wte_size);
    model->fc1_weights = read_parameter_array(model_file, fc1_weights_size);
    model->fc1_bias = read_parameter_array(model_file, fc1_bias_size);
    model->fc2_weights = read_parameter_array(model_file, fc2_weights_size);
    model->fc2_bias = read_parameter_array(model_file, fc2_bias_size);

    fcloseCheck(model_file);
}

void mlp_free(MLP *model) {
    free(model->wte);
    free(model->fc1_weights);
    free(model->fc1_bias);
    free(model->fc2_weights);
    free(model->fc2_bias);
}

// -----------------------------------------------------------------------------
// forward pass

void encoder_forward(float *act_emb, int *inputs, float *wte, int B, int T, int E) {
    for (int i = 0; i < B; i++) {
        for (int j = 0; j < T; j++) {
            int token = inputs[i*T + j];
            memcpy(&act_emb[(i*T + j)*E], &wte[token*E], E * sizeof(float));
        }
    }

}

void matmul_forward(float *c, float *a, float *b, float *bias, int m, int n, int k) {
    // TODO: check this works as expected
    // C = A * B + bias
    for (int i = 0; i < m; i++) {
        for (int j = 0; j < k; j++) {
            c[i*k + j] = 0;
            for (int l = 0; l < n; l++) {
                c[i*k + j] += a[i*n + l] * b[l*k + j];
            }

            // bias
            c[i*k + j] += bias[j];
        }
    }
}

void relu_forward(float *x, int size) {
    for (int i = 0; i < size; i++) {
        if (x[i] < 0) x[i] = 0;
    }
}

void softmax(float *probs, float *logits, int size, int batch_size) {
    for (int i = 0; i < batch_size; i++) {
        float max_val = -INFINITY;
        for (int j = 0; j < size; j++) {
            if (logits[i*size + j] > max_val) {
                max_val = logits[i*size + j];
            }
        }
        float sum = 0;
        for (int j = 0; j < size; j++) {
            probs[i*size + j] = exp(logits[i*size + j] - max_val);
            sum += probs[i*size + j];
        }
        for (int j = 0; j < size; j++) {
            probs[i*size + j] /= sum;
        }
    }
}

float cross_entropy(float *probs, int *targets, int vocab_size, int batch_size) {
    float loss = 0;
    for (int i = 0; i < batch_size; i++) {
        int target = targets[i];
        loss += -log(probs[i*vocab_size + target]);
    }
    return loss / batch_size;
}

float forward(MLP *model) {

    // ensure the model was initialized or error out
    if (model->wte == NULL) {
        printf("Error: model was not initialized properly.\n");
        exit(1);
    }

    // convenience shortcuts
    int B = model->batch_size;
    int T = model->context_length;
    int E = model->embedding_size;
    int H = model->hidden_size;
    int V = model->vocab_size;

    if (model->act_emb == NULL) {  // lazy initialization of activations the first time we call forward
        model->act_emb = (float*)mallocCheck(B * T * E * sizeof(float));
        model->act_h = (float*)mallocCheck(B * H * sizeof(float));
        model->act_logits = (float*)mallocCheck(B * V * sizeof(float));
        model->act_probs = (float*)mallocCheck(B * V * sizeof(float));
    }

    // encode all the tokens using the embedding table
    // inputs are the input tokens, (B, T) array of integers
    encoder_forward(model->act_emb, model->inputs, model->wte, B, T, E);

    // forward through the first linear layer
    matmul_forward(model->act_h, model->act_emb, model->fc1_weights, model->fc1_bias, B, T * E, H);
    relu_forward(model->act_h, B * H);

    // forward through the second linear layer
    matmul_forward(model->act_logits, model->act_h, model->fc2_weights, model->fc2_bias, B, H, V);  // (B, H) * (H, V) = (B, V)
    softmax(model->act_probs, model->act_logits, V, B);

    float loss = cross_entropy(model->act_probs, model->targets, V, B);
    return loss;
}

// -----------------------------------------------------------------------------
// backward pass

void crossentropy_softmax_backward(float* grad_logits, float* act_probs, int* targets, int B, int V) {
    // backwards through both softmax and crossentropy
    for (int b = 0; b < B; b++) {
        for (int i = 0; i < V; i++) {
            float p = act_probs[b*V + i];
            float indicator = i == targets[b] ? 1.0f : 0.0f;
            grad_logits[b*V + i] += (p - indicator) * (1 / B);
        }
    }
}

void matmul_backward(float* dinp, float* dweight, float* dbias,
                     const float* dout, const float* inp, const float* weight,
                     int B, int C, int OC) {
    // TODO: make sure this works
    // TODO: refactor
    // backward into inp first
    for (int b = 0; b < B; b++) {
        const float* dout_b = dout + b * OC;
        float* dinp_b = dinp + b * C;
        for (int o = 0; o < OC; o++) {
            const float* wrow = weight + o*C;
            float d = dout_b[o];
            for (int i = 0; i < C; i++) {
                dinp_b[i] += wrow[i] * d;
            }
        }
    }
    // backward into weight/bias
    for (int o = 0; o < OC; o++) {
        for (int b = 0; b < B; b++) {
            const float* dout_b = dout + b * OC;
            const float* inp_b = inp + b * C;
            float* dwrow = dweight + o*C;
            float d = dout_b[o];
            if (dbias != NULL) { dbias[o] += d; }
            for (int i = 0; i < C; i++) {
                dwrow[i] += inp_b[i] * d;
            }
        }
    }
}

void relu_backward(float* grad_x, float* grad_y, float* x, int size) {
    for (int i = 0; i < size; i++) {
        grad_x[i] = grad_y[i] * (x[i] > 0);
    }
}

void encoder_backward(float* dwte,
                      float* dout, int* inp,
                      int B, int T, int C) {
    for (int b = 0; b < B; b++) {
        for (int t = 0; t < T; t++) {
            float* dout_bt = dout + b * T * C + t * C;
            int ix = inp[b * T + t];
            float* dwte_ix = dwte + ix * C;
            for (int i = 0; i < C; i++) {
                float d = dout_bt[i];
                dwte_ix[i] += d;
            }
        }
    }
}

void backward(MLP *model) {
    // convenience shortcuts
    int B = model->batch_size;
    int T = model->context_length;
    int E = model->embedding_size;
    int H = model->hidden_size;
    int V = model->vocab_size;

    if (model->grad_logits == NULL) {  // lazy initialization of gradients the first time we call backward
        model->grad_logits = (float*)callocCheck(B * V, sizeof(float));
        model->grad_fc2_weights = (float*)callocCheck(V * H, sizeof(float));
        model->grad_fc2_bias = (float*)callocCheck(V, sizeof(float));
        model->grad_h = (float*)callocCheck(B * H, sizeof(float));
        model->grad_fc1 = (float*)callocCheck(B * H, sizeof(float));
        model->grad_fc1_weights = (float*)callocCheck(H * T * E, sizeof(float));
        model->grad_fc1_bias = (float*)callocCheck(H, sizeof(float));
        model->grad_emb = (float*)callocCheck(B * T * E, sizeof(float));
        model->grad_wte = (float*)callocCheck(V * E, sizeof(float));
    }

    crossentropy_softmax_backward(model->grad_logits, model->act_probs, model->targets, B, V);

    // backprop through the second linear layer
    matmul_backward(model->grad_h, model->grad_fc2_weights, model->grad_fc2_bias,
                    model->grad_logits, model->act_h, model->fc2_weights, B, H, V);

    // backprop through relu
    relu_backward(model->grad_fc1, model->grad_h, model->act_h, B * H);

    // backprop through the first linear layer
    matmul_backward(model->grad_emb, model->grad_fc1_weights, model->grad_fc1_bias,
                    model->grad_fc1, model->act_emb, model->fc1_weights, B, T * E, H);

    // backprop through the embedding layer
    encoder_backward(model->grad_wte, model->grad_emb, model->inputs, B, T, E);
}

// -----------------------------------------------------------------------------
// AdamW optimizer

typedef struct {
    float lr;
    float beta1;
    float beta2;
    float weight_decay;
    float eps;
    int t;
    float *m;
    float *v;
    MLP *model;
} AdamW;

AdamW* adamw_init(AdamW* optimizer, MLP *model, float lr, float beta1, float beta2, float weight_decay, float eps) {
    optimizer->lr = lr;
    optimizer->beta1 = beta1;
    optimizer->beta2 = beta2;
    optimizer->weight_decay = weight_decay;
    optimizer->eps = eps;
    optimizer->t = 0;  // timestep - used for bias correction
    optimizer->m = (float*)callocCheck(model->num_parameters, sizeof(float));
    optimizer->v = (float*)callocCheck(model->num_parameters, sizeof(float));
    optimizer->model = model;
}

void adamw_step(AdamW *optimizer, MLP* model) {
    optimizer->t += 1;
    float* grads = model->grad_logits;  // TODO: check whether this will work
    for (int i = 0; i < model->num_parameters; i++) {
        optimizer->m[i] = optimizer->beta1 * optimizer->m[i] + (1 - optimizer->beta1) * grads[i];
        optimizer->v[i] = optimizer->beta2 * optimizer->v[i] + (1 - optimizer->beta2) * grads[i] * grads[i];
        float m_hat = optimizer->m[i] / (1 - pow(optimizer->beta1, optimizer->t));
        float v_hat = optimizer->v[i] / (1 - pow(optimizer->beta2, optimizer->t));
        optimizer->model->wte[i] -= optimizer->lr * m_hat / (sqrt(v_hat) + optimizer->eps);
        if (optimizer->weight_decay > 0) {
            optimizer->model->wte[i] -= optimizer->lr * optimizer->weight_decay * optimizer->model->wte[i];
        }
    }
}

void adam_free(AdamW *optimizer) {
    free(optimizer->m);
    free(optimizer->v);
}

// -----------------------------------------------------------------------------
// simple DataLoader that iterates over all the n-grams

void dataloader(MLP* model, int *tokens, int context_length, int batch_size, int token_cnt) {
    if (model->act_emb == NULL) {  // lazy initialization the first time we call dataloader
        model->inputs = (int*)mallocCheck(batch_size * model->context_length * sizeof(int));
        model->targets = (int*)mallocCheck(batch_size * sizeof(int));
    }

    static int pos = 0;  // static variable persists between calls i.e. only initialized once
    for (int i = 0; i < batch_size; i++) {
        for (int j = 0; j < context_length; j++) {
            model->inputs[i*context_length + j] = tokens[pos + j];
        }
        model->targets[i] = tokens[pos + context_length];
        pos = pos + 1;
        if (pos + context_length >= token_cnt) {
            pos = 0;
        }
    }
}

// -----------------------------------------------------------------------------
// evaluation function

float eval_split(MLP *model, int *tokens, int token_cnt, int max_batches, int batch_size) {
    float total_loss = 0;
    int num_batches = token_cnt / batch_size;
    num_batches = max_batches ? min(num_batches, max_batches) : num_batches;
    for (int i = 0; i < num_batches; i++) {
        dataloader(&model, tokens, model->context_length, batch_size, token_cnt);
        float loss = forward(model);
        total_loss += loss;
    }
    float mean_loss = total_loss / num_batches;
    return mean_loss;
}

// -----------------------------------------------------------------------------
// let's train!

int main() {
    FILE *train_file = fopenCheck("data/train.txt", "r");

    // ensure that the training data only contains lowercase letters and newlines
    char c;
    while ((c = fgetc(train_file)) != EOF) {
        if (c != '\n' && !(c >= 'a' && c <= 'z')) {
            fprintf(stderr, "Error: Invalid character in training data: %c\n", c);
            exit(EXIT_FAILURE);
        }
    }

    fseek(train_file, 0, SEEK_SET);  // Reset the train txt file pointer
    FILE *val_file = fopenCheck("data/val.txt", "r");
    FILE *test_file = fopenCheck("data/test.txt", "r");

    // allocate memory for the tokens
    int TRAIN_SIZE = 213796;
    int VAL_SIZE = 7179;
    int TEST_SIZE = 7170;
    int train_tokens[TRAIN_SIZE];
    int val_tokens[VAL_SIZE];
    int test_tokens[TEST_SIZE];

    int train_token_count = 0;
    int val_token_count = 0;
    int test_token_count = 0;

    // pre-tokenize all the splits one time up here
    // \n -> 0, a -> 1, b -> 2, ..., z -> 26
    while ((c = fgetc(train_file)) != EOF) {
        train_tokens[train_token_count++] = c == '\n' ? 0 : c - 'a' + 1;
    }
    while ((c = fgetc(val_file)) != EOF) {
        val_tokens[val_token_count++] = c == '\n' ? 0 : c - 'a' + 1;
    }
    while ((c = fgetc(test_file)) != EOF) {
        test_tokens[test_token_count++] = c == '\n' ? 0 : c - 'a' + 1;
    }

    // close the files
    fcloseCheck(train_file);
    fcloseCheck(val_file);
    fcloseCheck(test_file);

    // create the model
    MLP model;
    mlp_build_from_checkpoint(&model, "mlp_weights.bin");
    int context_length = model.context_length;
    int embedding_size = model.embedding_size;
    int hidden_size = model.hidden_size;

    // optimizer
    float learning_rate = 1e-3;
    AdamW optimizer;
    float weight_decay = 1e-4;
    float beta1 = 0.9;
    float beta2 = 0.999;
    float eps = 1e-8;
    adamw_init(&optimizer, &model, learning_rate, beta1, beta2, weight_decay, eps);

    // training loop
    int batch_size = 64;
    int num_steps = 50000;
    model.batch_size = batch_size;
    printf("num_steps %d, num_epochs %.2f\n", num_steps, num_steps * batch_size / (float)train_token_count);
    for (int step = 0; step < num_steps; step++) {
        // cosine learning rate schedule, from max lr to 0
        float lr = learning_rate * 0.5 * (1 + cos(M_PI * step / num_steps));
        // every now and then evaluate the validation loss
        int last_step = step == num_steps - 1;
        if (step % 200 == 0 || last_step) {
            float train_loss = eval_split(&model, train_tokens, train_token_count, 20, batch_size);
            float val_loss = eval_split(&model, val_tokens, val_token_count, 0, batch_size);
            printf("step %d/%d | train_loss %.4f | val_loss %.4f | lr %.6f\n", step, num_steps, train_loss, val_loss, lr);
        }

        // get the next batch of training data
        dataloader(&model, train_tokens, context_length, batch_size, train_token_count);
        // forward through the model
        float loss = forward(&model);
        // backpropagate
        backward(&model);
        // step the optimizer - update the weights
        adamw_step(optimizer, grads, sizeof(grads) / sizeof(grads[0]));
    }

    // TODO: free up all the memory
    adam_free(&optimizer);
    mlp_free(&model);
    return 0;
}