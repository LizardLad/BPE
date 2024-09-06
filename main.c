#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <stdbool.h>

#define SKIP_TOKEN UINT16_MAX

struct vocab_file_header_t {
    char preamble[5]; //{'V', 'O', 'C', 'A', 'B'}
    bool complete;
    uint16_t len;
    uint16_t desired_len;
};

enum MODE {
    MODE_STATUS,
    MODE_TRAIN
};

struct command_line_args {
    enum MODE mode;
    char *project;
    uint16_t vocab_size;
    uint8_t file_count;
    char *vocab_path;
    char **filepaths;
};

struct vocab {
    uint16_t b1;
    uint16_t b2;
    uint16_t rep; //Replacement character for above pair
};

int parse_argv(int argc, char **argv, struct command_line_args *command_line_args) {
    char *stat_p = argv[0];
    while(*stat_p) {
        *stat_p = toupper(*stat_p);
        stat_p++;
    } if(strlen(argv[0]) < 5) {
        fprintf(stderr, "Mode not train or status\n");
        return 1;
    }

    
    if(strncmp(argv[0], "TRAIN", 5) == 0) {
        command_line_args->project = argv[1];
        command_line_args->mode = MODE_TRAIN;
    } else if(strncmp(argv[0], "STATUS", 6) == 0) {
        command_line_args->vocab_path = argv[1];
        command_line_args->mode = MODE_STATUS;
        return 0;
    } else {
        fprintf(stderr, "Not TRAIN or STATUS, instead mode requested is %s\n", argv[0]);
        return 2;
    }
    if(argc < 5) {
        fprintf(stderr, "Not enough arguments for train mode\n");
        return 3;
    }

    if(sscanf(argv[2], "%hu", &(command_line_args->vocab_size)) != 1) {
        fprintf(stderr, "Could not parse the vocab count as an unsigned short\n");
        return 4;
    }

    command_line_args->vocab_path = argv[3];

    command_line_args->file_count = argc - 4;
    command_line_args->filepaths = &(argv[4]);
    return 0;
}

int apply_vocab(struct command_line_args command_line_args, struct vocab *vocab, uint16_t vocab_len, uint16_t *buff, size_t buff_len) {
    for(int i = 0; i < vocab_len; i++) {
        for(size_t j = 0; j < buff_len-1;) {
            if(buff[j] == SKIP_TOKEN) {
                continue;
            }
            //Find byte 2
            size_t p = 1;
            while(j+p < buff_len) {
                if(buff[j+p] != SKIP_TOKEN) {
                    break;
                }
                p++;
            }
            if(buff[j+p] == buff_len) {
                continue;
            }
            if(buff[j] == vocab[i].b1 && vocab[i].b2 == buff[j+p]) {
                //Perform replacement
                buff[j] = vocab[i].rep;
                buff[j+p] = SKIP_TOKEN;
            } else {
                j += p;
            }
        }
    }
    return 0;
}

int increment_byte_pairs(struct command_line_args command_line_args, size_t **byte_pairs, uint16_t b1, uint16_t b2) {
    if(byte_pairs[b1] == NULL) { //Haven't had this as the first byte in the map yet, allocate it
        byte_pairs[b1] = (size_t *)calloc(command_line_args.vocab_size, sizeof(size_t));
        if(byte_pairs[b1] == NULL) {
            fprintf(stderr, "Failed to allocate enough memory to the map, failing\n");
            exit(4);
        }
    }
    byte_pairs[b1][b2]++;
}

int count_pairs(struct command_line_args command_line_args, size_t **byte_pairs, uint16_t *buff, size_t len) {
    for(size_t i = 0; i < len; i++) {
        if(buff[i] == SKIP_TOKEN) {
            continue;
        }

        //Get second non-skipable byte
        size_t p = 1;
        while(i+p < len) {
            if(buff[i+p] != SKIP_TOKEN) {
                break;
            }
            p++;
        }
        if(i+p == len) {
            return 0;
        }

        increment_byte_pairs(command_line_args, byte_pairs, buff[i], buff[i+p]);
        i += p-1;
    }
    return 0;
}

struct vocab max_pair(struct command_line_args command_line_args, size_t **byte_pairs) {
    size_t max_occurances = 0;
    struct vocab vocab = {.b1 = 0, .b2 = 0, .rep = 0};
    for(uint16_t b1 = 0; b1 < command_line_args.vocab_size; b1++) {
        if(byte_pairs[b1] == NULL) {
            continue;
        }
        for(uint16_t b2 = 0; b2 < command_line_args.vocab_size; b2++) {
            if(byte_pairs[b1][b2] > max_occurances) {
                max_occurances = byte_pairs[b1][b2];
                vocab.b1 = b1;
                vocab.b2 = b2;
            }
        }
    }
    return vocab;
}

int zero_count(struct command_line_args command_line_args, size_t **byte_pairs) {
    for(uint16_t b1 = 0; b1 < command_line_args.vocab_size; b1++) {
        if(byte_pairs[b1] == NULL) {
            continue;
        }
        for(uint16_t b2 = 0; b2 < command_line_args.vocab_size; b2++) {
            byte_pairs[b1][b2] = 0;
        }
    }
    return 0;
}

int train(struct command_line_args command_line_args) {
    if(command_line_args.vocab_size <= 256) {
        fprintf(stderr, "Vocab count too low\n");
        exit(7);
    }

    size_t **byte_pairs = (size_t **)calloc(command_line_args.vocab_size, sizeof(size_t *));
    if(byte_pairs == NULL) {
        fprintf(stderr, "Failed to allocate enough memory\n");
        exit(3);
    }

    struct vocab *vocab = (struct vocab*)calloc(command_line_args.vocab_size - 256, sizeof(struct vocab));
    uint16_t vocab_len = 0; //Current length
    if(vocab == NULL) {
        fprintf(stderr, "Failed to allocate enough memory\n");
        exit(6);
    }
    
    //Init the vocab file for status calls
    char *vocab_filename = command_line_args.vocab_path;
    FILE *vocab_file = fopen(vocab_filename, "w+");
    struct vocab_file_header_t header = {.preamble = {'V', 'O', 'C', 'A', 'B'}, .complete=false, .len=0, .desired_len=command_line_args.vocab_size};
    fwrite(&header, sizeof(struct vocab_file_header_t), 1, vocab_file);
    fflush(vocab_file);
    fclose(vocab_file);

    //Start reading the files
    for(uint16_t i = 0; i < command_line_args.vocab_size-256; i++) {
        for(uint8_t j = 0; j < command_line_args.file_count; j++) {
            if(access(command_line_args.filepaths[j], F_OK)) {
                //File does not exist
                fprintf(stderr, "File does not exist, continuing\n");
                continue;
            }

            FILE *file = fopen(command_line_args.filepaths[j], "rb");
            fseek(file, 0, SEEK_END);
            size_t len = ftell(file);
            fseek(file, 0, SEEK_SET);
            uint16_t *buff = (uint16_t *)calloc(len, sizeof(uint16_t));
            if(buff == NULL) {
                fprintf(stderr, "Failed to allocate memory for file\n");
                exit(5);
            }
            for(size_t k = 0; k < len; k++) {
                fread(&(buff[k]), 1, 1, file); //1 byte into uint16_t
            }
            fclose(file);

            printf("Start file %s pair count\n", command_line_args.filepaths[j]);
            apply_vocab(command_line_args, vocab, vocab_len, buff, len);
            count_pairs(command_line_args, byte_pairs, buff, len);
            printf("Finish file %s pair count\n", command_line_args.filepaths[j]);

            free(buff);
        }
        //Find max pair
        struct vocab pair = max_pair(command_line_args, byte_pairs);
        if(pair.b1 == 0 && pair.b2 == 0) {
            printf("Early stopping... Only one token left\n");
            printf("Vocab size: %hu\n", vocab_len);
            break;
        }
        pair.rep = i + 256;
        vocab[i] = pair;
        vocab_len++;

        //Update the file
        FILE *vocab_file = fopen(vocab_filename, "r+");
        fseek(vocab_file, 0, SEEK_END);
        fwrite(&pair, sizeof(struct vocab), 1, vocab_file);
        fseek(vocab_file, 0, SEEK_SET);
        header.len = vocab_len;
        fwrite(&header, sizeof(struct vocab_file_header_t), 1, vocab_file);
        fflush(vocab_file);
        fclose(vocab_file);

        printf("Most common pair in last run: (%hu, %hu), replaced with %hu\n", pair.b1, pair.b2, pair.rep);

        //Zero for next pass
        zero_count(command_line_args, byte_pairs); 
    }

    vocab_file = fopen(vocab_filename, "r+");
    fseek(vocab_file, 0, SEEK_SET);
    header.len = vocab_len;
    header.complete = true;
    fwrite(&header, sizeof(struct vocab_file_header_t), 1, vocab_file);
    fflush(vocab_file);
    fclose(vocab_file);

    for(uint16_t i = 0; i < command_line_args.vocab_size; i++) {
        if(byte_pairs[i] != NULL) {
            free(byte_pairs[i]);
        }
    }
    free(byte_pairs);

    free(vocab);

    return 0;
}

int status(struct command_line_args command_line_args) {
    //Read from file and export on stdout as required
    char *vocab_filename = command_line_args.vocab_path;
    FILE *vocab_file = fopen(vocab_filename, "r");
    if(vocab_file == NULL) {
        perror("[CRITICAL] Failed to open file: ");
        exit(11);
    }
    
    struct vocab_file_header_t header;
    fread(&header, sizeof(struct vocab_file_header_t), 1, vocab_file);
    if(strncmp(header.preamble, "VOCAB", 5)) {
        fprintf(stderr, "[CRITICAL] File has been corrupted\n");
        exit(10);
    }

    struct vocab *vocab = calloc(header.len, sizeof(struct vocab));
    if(vocab == NULL) {
        fprintf(stderr, "[CRITICAL] Failed to allocate enough memory\n");
        exit(12);
    }

    size_t bytes_read = fread(vocab, sizeof(struct vocab), header.len, vocab_file);
    if(bytes_read != (uint64_t)header.len) {
        fprintf(stderr, "[ERROR] Error reading the file. Read %lu of %lu, %hu records\n", bytes_read, header.len);
        exit(20);
    }

    fclose(vocab_file);

    //All vocab is in memory now
    //Just have to read it out back to the calling function
    fprintf(stdout, "{\"complete\":%s,\"desired_size\":%hu,\"vocab\":[", header.complete ? "true" : "false", header.desired_len);
    for(uint16_t i = 0; i < header.len; i++) {
        fprintf(stdout, "{\"b1\":%hu,\"b2\":%hu,\"rep\":%hu}", vocab[i].b1, vocab[i].b2, vocab[i].rep);
        if(i != header.len-1) {
            fprintf(stdout, ",");
        }
    }
    fprintf(stdout, "]}\n");

    return 0;
}

int main(int argc, char **argv, char **envp) {
    if(argc < 3) {
        fprintf(stderr, "Incorrect number of arguments provided, expect at least mode and project\n");
        exit(1);
    }

    //Argument 0 is the executable
    //Argument 1 is the mode
    // If Argument 1 is train then
    //   Argument 2 is the project
    //   Argument 3 is the target vocab size
    //   Argument 4 is the output file
    //   Argument 5.. are the filepaths to train on
    // Else if Argument 1 is status
    //   Argument 2 is the project
    struct command_line_args command_line_args;
    if(parse_argv(--argc, ++argv, &command_line_args)) {
        exit(2); //Error already printed in the parse_argv function;
    }

    switch(command_line_args.mode) {
    case MODE_TRAIN:
        train(command_line_args);
        break;
    
    case MODE_STATUS:
        status(command_line_args);
        break;

    default: //Can't happen, was validated in parse_argv()
        fprintf(stderr, "Bad mode\n");
        break;
    }
    return 0;
}