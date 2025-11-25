# EvalScoreModelGrader

A ScoreModelGrader object that uses a model to assign a score to the input. 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The object type, which is always &#x60;score_model&#x60;. | 
**name** | **str** | The name of the grader. | 
**model** | **str** | The model to use for the evaluation. | 
**sampling_params** | **object** | The sampling parameters for the model. | [optional] 
**input** | [**List[EvalItem]**](EvalItem.md) | The input text. This may include template strings. | 
**pass_threshold** | **float** | The threshold for the score. | [optional] 
**range** | **List[float]** | The range of the score. Defaults to &#x60;[0, 1]&#x60;. | [optional] 

## Example

```python
from openapi_client.models.eval_score_model_grader import EvalScoreModelGrader

# TODO update the JSON string below
json = "{}"
# create an instance of EvalScoreModelGrader from a JSON string
eval_score_model_grader_instance = EvalScoreModelGrader.from_json(json)
# print the JSON string representation of the object
print(EvalScoreModelGrader.to_json())

# convert the object into a dict
eval_score_model_grader_dict = eval_score_model_grader_instance.to_dict()
# create an instance of EvalScoreModelGrader from a dict
eval_score_model_grader_from_dict = EvalScoreModelGrader.from_dict(eval_score_model_grader_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


