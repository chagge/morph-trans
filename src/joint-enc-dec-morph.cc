#include "joint-enc-dec-morph.h"

using namespace std;
using namespace cnn;
using namespace cnn::expr;

string BOW = "<s>", EOW = "</s>";
unsigned MAX_PRED_LEN = 100;

JointEncDecMorph::JointEncDecMorph(
  const unsigned& char_length, const unsigned& hidden_length,
  const unsigned& vocab_length, const unsigned& num_layers,
  const unsigned& num_morph, vector<Model*>* m,
  vector<AdadeltaTrainer>* optimizer) {
  char_len = char_length;
  hidden_len = hidden_length;
  vocab_len = vocab_length;
  layers = num_layers;
  morph_len = num_morph;
  InitParams(m);
}

void JointEncDecMorph::InitParams(vector<Model*>* m) {
  // Have all the shared parameters in one model
  input_forward = LSTMBuilder(layers, char_len, hidden_len, (*m)[morph_len]);
  input_backward = LSTMBuilder(layers, char_len, hidden_len, (*m)[morph_len]);
  output_forward = LSTMBuilder(layers, 2 * char_len + hidden_len,
                               hidden_len, (*m)[morph_len]);

  char_vecs = (*m)[morph_len]->add_lookup_parameters(vocab_len, {char_len});

  phidden_to_output = (*m)[morph_len]->add_parameters({vocab_len, hidden_len});
  phidden_to_output_bias = (*m)[morph_len]->add_parameters({vocab_len, 1});

  eps_vecs = (*m)[morph_len]->add_lookup_parameters(max_eps, {char_len});

  for (unsigned i = 0; i < morph_len; ++i) {
    ptransform_encoded.push_back((*m)[i]->add_parameters({hidden_len,
                                                          2 * hidden_len}));
    ptransform_encoded_bias.push_back((*m)[i]->add_parameters({hidden_len, 1}));
  }
}

void JointEncDecMorph::AddParamsToCG(const unsigned& morph_id, ComputationGraph* cg) {
  input_forward.new_graph(*cg);
  input_backward.new_graph(*cg);
  output_forward.new_graph(*cg);

  hidden_to_output = parameter(*cg, phidden_to_output);
  hidden_to_output_bias = parameter(*cg, phidden_to_output_bias);

  transform_encoded = parameter(*cg, ptransform_encoded[morph_id]);
  transform_encoded_bias = parameter(*cg, ptransform_encoded_bias[morph_id]);
}

void JointEncDecMorph::RunFwdBwd(const vector<unsigned>& inputs,
                              Expression* hidden, ComputationGraph *cg) {
  vector<Expression> input_vecs;
  for (const unsigned& input_id : inputs) {
    input_vecs.push_back(lookup(*cg, char_vecs, input_id));
  }

  // Run forward LSTM
  Expression forward_unit;
  input_forward.start_new_sequence();
  for (unsigned i = 0; i < input_vecs.size(); ++i) {
    forward_unit = input_forward.add_input(input_vecs[i]);
  }

  // Run backward LSTM
  Expression backward_unit;
  input_backward.start_new_sequence();
  for (int i = input_vecs.size() - 1; i >= 0; --i) {
    backward_unit = input_backward.add_input(input_vecs[i]);
  }

  // Concatenate the forward and back hidden layers
  *hidden = concatenate({forward_unit, backward_unit});
}

void JointEncDecMorph::TransformEncodedInput(Expression* encoded_input) const {
  *encoded_input = affine_transform({transform_encoded_bias,
                                     transform_encoded, *encoded_input});
}

void JointEncDecMorph::ProjectToOutput(const Expression& hidden, Expression* out)
  const {
  *out = affine_transform({hidden_to_output_bias, hidden_to_output, hidden});
}


Expression JointEncDecMorph::ComputeLoss(const vector<Expression>& hidden_units,
                                      const vector<unsigned>& targets) const {
  assert(hidden_units.size() == targets.size());
  vector<Expression> losses;
  for (unsigned i = 0; i < hidden_units.size(); ++i) {
    Expression out;
    ProjectToOutput(hidden_units[i], &out);
    losses.push_back(pickneglogsoftmax(out, targets[i]));
  }
  return sum(losses);
}

float JointEncDecMorph::Train(const unsigned& morph_id, const vector<unsigned>& inputs,
                           const vector<unsigned>& outputs, AdadeltaTrainer* opt,
                           AdadeltaTrainer* shared_opt) {
  ComputationGraph cg;
  AddParamsToCG(morph_id, &cg);

  // Encode and Transform to feed into decoder
  Expression encoded_input_vec;
  RunFwdBwd(inputs, &encoded_input_vec, &cg);
  TransformEncodedInput(&encoded_input_vec);

  // Use this encoded word vector to predict the transformed word
  vector<Expression> input_vecs_for_dec;
  vector<unsigned> output_ids_for_pred;
  for (unsigned i = 0; i < outputs.size(); ++i) {
    if (i < outputs.size() - 1) { 
      // '</s>' will not be fed as input -- it needs to be predicted.
      if (i < inputs.size() - 1) {
        input_vecs_for_dec.push_back(concatenate(
            {encoded_input_vec, lookup(cg, char_vecs, outputs[i]),
             lookup(cg, char_vecs, inputs[i + 1])}));
      } else {
        input_vecs_for_dec.push_back(concatenate(
            {encoded_input_vec, lookup(cg, char_vecs, outputs[i]),
             lookup(cg, eps_vecs, min(unsigned(i - inputs.size()), max_eps - 1))}));
      }
    }
    if (i > 0) {  // '<s>' will not be predicted in the output -- its fed in.
      output_ids_for_pred.push_back(outputs[i]);
    }
  }

  vector<Expression> decoder_hidden_units;
  output_forward.start_new_sequence();
  for (const auto& vec : input_vecs_for_dec) {
    decoder_hidden_units.push_back(output_forward.add_input(vec));
  }
  Expression loss = ComputeLoss(decoder_hidden_units, output_ids_for_pred);

  float return_loss = as_scalar(cg.forward());
  cg.backward();
  opt->update(1.0f);  // Update the morph specific parameters
  shared_opt->update(1.0f);  // Update the shared parameters
  return return_loss;
}

void Serialize(string& filename, JointEncDecMorph& model,
               vector<Model*>* cnn_models) {
  ofstream outfile(filename);
  if (!outfile.is_open()) {
    cerr << "File opening failed" << endl;
  }

  boost::archive::text_oarchive oa(outfile);
  oa & model;
  for (unsigned i = 0; i < cnn_models->size(); ++i) {
    oa & *(*cnn_models)[i];
  }

  cerr << "Saved model to: " << filename << endl;
  outfile.close();
}

void Read(string& filename, JointEncDecMorph* model, vector<Model*>* cnn_models) {
  ifstream infile(filename);
  if (!infile.is_open()) {
    cerr << "File opening failed" << endl;
  }

  boost::archive::text_iarchive ia(infile);
  ia & *model;
  for (unsigned i = 0; i < model->morph_len + 1; ++i) {
    Model *cnn_model = new Model();
    cnn_models->push_back(cnn_model);
  }

  model->InitParams(cnn_models);
  for (unsigned i = 0; i < cnn_models->size(); ++i) {
    ia & *(*cnn_models)[i];
  }

  cerr << "Loaded model from: " << filename << endl;
  infile.close();
}

void
EnsembleDecode(const unsigned& morph_id, unordered_map<string, unsigned>& char_to_id,
               const vector<unsigned>& input_ids, vector<unsigned>* pred_target_ids,
               vector<JointEncDecMorph*>* ensmb_model) {
  ComputationGraph cg;

  unsigned ensmb = ensmb_model->size();
  vector<Expression> encoded_word_vecs;
  for (unsigned i = 0; i < ensmb; ++i) {
    Expression encoded_word_vec;
    auto model = (*ensmb_model)[i];
    model->AddParamsToCG(morph_id, &cg);
    model->RunFwdBwd(input_ids, &encoded_word_vec, &cg);
    model->TransformEncodedInput(&encoded_word_vec);
    encoded_word_vecs.push_back(encoded_word_vec);
    model->output_forward.start_new_sequence();
  }

  unsigned out_index = 1;
  unsigned pred_index = char_to_id[BOW];
  while (pred_target_ids->size() < MAX_PRED_LEN) {
    vector<Expression> ensmb_out;
    pred_target_ids->push_back(pred_index);
    if (pred_index == char_to_id[EOW]) {
      return;  // If the end is found, break from the loop and return
    }

    for (unsigned ensmb_id = 0; ensmb_id < ensmb; ++ensmb_id) {
      auto model = (*ensmb_model)[ensmb_id];
      Expression prev_output_vec = lookup(cg, model->char_vecs, pred_index);
      Expression input, input_char_vec;
      if (out_index < input_ids.size()) {
        input_char_vec = lookup(cg, model->char_vecs, input_ids[out_index]);
      } else {
        input_char_vec = lookup(cg, model->eps_vecs,
                                min(unsigned(out_index - input_ids.size()),
                                             model->max_eps - 1));
      }
      input = concatenate({encoded_word_vecs[ensmb_id], prev_output_vec,
                           input_char_vec});

      Expression hidden = model->output_forward.add_input(input);
      Expression out;
      model->ProjectToOutput(hidden, &out);
      ensmb_out.push_back(log_softmax(out));
    }

    Expression out = sum(ensmb_out) / ensmb_out.size();
    vector<float> dist = as_vector(cg.incremental_forward());
    pred_index = distance(dist.begin(), max_element(dist.begin(), dist.end()));
    out_index++;
  }
}
