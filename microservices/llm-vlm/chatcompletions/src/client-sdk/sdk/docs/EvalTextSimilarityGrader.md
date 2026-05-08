# EvalTextSimilarityGrader

A TextSimilarityGrader object which grades text based on similarity metrics. 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of grader. | [default to 'text_similarity']
**name** | **str** | The name of the grader. | [optional] 
**input** | **str** | The text being graded. | 
**reference** | **str** | The text being graded against. | 
**pass_threshold** | **float** | A float score where a value greater than or equal indicates a passing grade. | 
**evaluation_metric** | **str** | The evaluation metric to use. One of &#x60;fuzzy_match&#x60;, &#x60;bleu&#x60;, &#x60;gleu&#x60;, &#x60;meteor&#x60;, &#x60;rouge_1&#x60;, &#x60;rouge_2&#x60;, &#x60;rouge_3&#x60;, &#x60;rouge_4&#x60;, &#x60;rouge_5&#x60;, or &#x60;rouge_l&#x60;. | 

## Example

```python
from openapi_client.models.eval_text_similarity_grader import EvalTextSimilarityGrader

# TODO update the JSON string below
json = "{}"
# create an instance of EvalTextSimilarityGrader from a JSON string
eval_text_similarity_grader_instance = EvalTextSimilarityGrader.from_json(json)
# print the JSON string representation of the object
print(EvalTextSimilarityGrader.to_json())

# convert the object into a dict
eval_text_similarity_grader_dict = eval_text_similarity_grader_instance.to_dict()
# create an instance of EvalTextSimilarityGrader from a dict
eval_text_similarity_grader_from_dict = EvalTextSimilarityGrader.from_dict(eval_text_similarity_grader_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


