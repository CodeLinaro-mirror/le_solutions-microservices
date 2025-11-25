# EvalLabelModelGrader

A LabelModelGrader object which uses a model to assign labels to each item in the evaluation. 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The object type, which is always &#x60;label_model&#x60;. | 
**name** | **str** | The name of the grader. | 
**model** | **str** | The model to use for the evaluation. Must support structured outputs. | 
**input** | [**List[EvalItem]**](EvalItem.md) |  | 
**labels** | **List[str]** | The labels to assign to each item in the evaluation. | 
**passing_labels** | **List[str]** | The labels that indicate a passing result. Must be a subset of labels. | 

## Example

```python
from openapi_client.models.eval_label_model_grader import EvalLabelModelGrader

# TODO update the JSON string below
json = "{}"
# create an instance of EvalLabelModelGrader from a JSON string
eval_label_model_grader_instance = EvalLabelModelGrader.from_json(json)
# print the JSON string representation of the object
print(EvalLabelModelGrader.to_json())

# convert the object into a dict
eval_label_model_grader_dict = eval_label_model_grader_instance.to_dict()
# create an instance of EvalLabelModelGrader from a dict
eval_label_model_grader_from_dict = EvalLabelModelGrader.from_dict(eval_label_model_grader_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


